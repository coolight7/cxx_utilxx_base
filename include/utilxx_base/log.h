#pragma once

#include "fmt/format.h"
#include "utilxx_base/export.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace utilxx_base {

/// 日志级别
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Out,
};

/// 日志条目: 格式化一次, 经 shared_ptr<const LogEntry> 共享给所有 sink,
/// sink 只读不可 move, 保证每个 sink 都能拿到完整内容
struct UTILXX_BASE_API LogEntry {
    LogLevel    level;
    uint64_t    seq;     ///< 全局递增序号 (入队时分配, 用于排序)
    int64_t     wallNs;  ///< 墙钟时间 ns since epoch (入队时打, 反映产生时刻)
    /// 模块名 (产生日志的源文件基名, 或调用方显式给的标签; 空 = 未指定)
    /// - XX_LOG* 宏自动填 [logModuleOf] (__FILE__) 的结果; 按模块调级别见
    ///   [LogDispatcher::setModuleLevel]
    std::string module;
    std::string message; ///< 已格式化的日志内容
};

/// 由源文件路径取"模块名" (文件名去掉扩展名; 目录分隔符兼容 `/` 与 `\`)
/// - 编译期常量求值: 宏里直接传 `__FILE__`, 无运行期分配
/// - 例: `D:/proj/lib/src/agent/session_store.cpp` -> `session_store`
constexpr std::string_view logModuleOf(std::string_view file) noexcept {
    const auto slash = file.find_last_of("/\\");
    auto       base  = (slash == std::string_view::npos) ? file : file.substr(slash + 1);
    const auto dot   = base.find_last_of('.');
    return (dot == std::string_view::npos) ? base : base.substr(0, dot);
}

/// 日志接收基类
/// - 内置线程安全有界队列: 生产者线程调 enqueue() 入队, 宿主线程调 pump() 处理
/// - onLog() 总在宿主线程串行执行, 子类无需自行加锁
/// - 队列满时丢弃新条目并计数, 保证生产者永不阻塞
class UTILXX_BASE_API LogSink {
public:

    virtual ~LogSink() = default;

    /// 生产者线程调用, 线程安全入队 (满则丢弃)
    void enqueue(std::shared_ptr<const LogEntry> entry);

    /// 宿主线程调用, 排空队列并逐条调 onLog; 返回处理条数
    size_t pump();

    /// 等待队列排空 (默认循环调 pump; ThreadedLogSink 覆写为等待后台线程)
    virtual void flush();

    /// 已丢弃的条目累计数
    uint64_t droppedCount() const {
        return dropped_.load(std::memory_order_relaxed);
    }

protected:

    /// 宿主线程串行调用, 子类实现具体输出逻辑 (无需加锁)
    virtual void onLog(const LogEntry& entry) = 0;

    /// 队列溢出丢弃时调用 (默认写 stderr; 子类可覆写, 如 TUI 显示提示)
    virtual void onDropped(uint64_t count);

    std::mutex                                  mutex_;
    std::condition_variable                     cv_;
    std::deque<std::shared_ptr<const LogEntry>> queue_;
    size_t                                      maxQueue_ = 4096;
    std::atomic<uint64_t>                       dropped_{0};
};

/// 自带后台处理线程的 LogSink
/// - 构造时启动线程, 析构时停止并 drain 剩余日志
/// - 适用于 stderr 输出、网络转发等无需绑定特定线程的 sink
class UTILXX_BASE_API ThreadedLogSink : public LogSink {
public:

    ThreadedLogSink();
    ~ThreadedLogSink() override;

    ThreadedLogSink(const ThreadedLogSink&)            = delete;
    ThreadedLogSink& operator=(const ThreadedLogSink&) = delete;

    /// 等待后台线程处理完队列中所有条目
    void flush() override;

protected:

    /// 停止后台线程: 等待队列排空且线程空闲后停止并 join
    /// - 必须在最派生类析构中调用 (此时虚表仍为最派生类): 线程执行 onLog
    ///   虚调用解析安全; 若延迟到基类析构 (虚表已切换为基类), 线程执行纯虚
    ///   onLog 虚调用会 purecall -> abort (进程退出瞬间日志刚入队时最易触发)
    /// - 基类析构会幂等兜底 (已 join 后 joinable() 为 false 直接跳过)
    void shutdownThread();

    void onLog(const LogEntry& entry) override = 0;

private:

    void threadLoop();

    /// 后台线程读写的状态必须声明在 `thread_` **之前**:
    /// 成员按声明顺序初始化, `thread_` 的构造会立刻启动线程并在其中读 `running_`/
    /// `idle_`; 若这两个成员排在 `thread_` 之后, 线程可能在它们被初始化前读到
    /// 未初始化的值 (TSan 可复现的数据竞争, 且线程可能直接判定为"已停止"而退出,
    /// 丢掉后续日志)。
    bool        running_ = true; // 受 LogSink::mutex_ 保护
    bool        idle_    = true; // 受 LogSink::mutex_ 保护; 线程未在 onLog 中时为 true
    std::thread thread_;
};

/// std::atomic<std::shared_ptr<T>> 的可移植封装:
/// - 主流平台直接用标准原子特化, 读路径完全无锁
/// - Android NDK libc++ / llvm-mingw libc++ 未实现该特化 (primary template
///   要求 trivially copyable, shared_ptr 不满足), 退化为 mutex 保护的普通
///   shared_ptr: 写路径 (sink 注册/移除) 罕见且持锁短, dispatch 热路径实际
///   接近无锁
/// - 检测: _LIBCPP_VERSION 表示使用 libc++ (不论 Android/Windows/Linux),
///   其 atomic<shared_ptr> 特化在部分版本/配置下缺失, 统一回退
template<typename T>
class AtomicSharedPtr {
public:

    AtomicSharedPtr() = default;

    explicit AtomicSharedPtr(std::shared_ptr<T> v) :
        value_(std::move(v)) {}

    [[nodiscard]] std::shared_ptr<T>
        load(std::memory_order order = std::memory_order_seq_cst) const {
#if XX_IS_ANDROID_D || defined(_LIBCPP_VERSION)
        (void)order; // 锁本身已提供所需的同步语义
        std::lock_guard<std::mutex> lock(mutex_);
        return value_;
#else
        return value_.load(order);
#endif
    }

    void store(std::shared_ptr<T> desired, std::memory_order order = std::memory_order_seq_cst) {
#if XX_IS_ANDROID_D || defined(_LIBCPP_VERSION)
        (void)order;
        std::lock_guard<std::mutex> lock(mutex_);
        value_ = std::move(desired);
#else
        value_.store(std::move(desired), order);
#endif
    }

private:

#if XX_IS_ANDROID_D || defined(_LIBCPP_VERSION)
    mutable std::mutex mutex_;
    std::shared_ptr<T> value_;
#else
    std::atomic<std::shared_ptr<T>> value_;
#endif
};

/// 全局日志分发器 (单例)

/// - 线程安全
/// - XX_LOG 宏将日志格式化后入队到所有已注册的 sink (非阻塞)
/// - sink 以 weak_ptr 持有, 注册方需自行持有 shared_ptr 以保持其有效
/// - 性能: dispatch 为最热路径, 采用 copy-on-write 快照实现无锁读取;
///   仅 addSink/removeSink (罕见) 在 mutex_ 下复制并原子替换快照
class UTILXX_BASE_API LogDispatcher {
public:

    static LogDispatcher& instance();

    ~LogDispatcher();

    void addSink(std::shared_ptr<LogSink> sink);

    void removeSink(const std::shared_ptr<LogSink>& sink);

    /// 创建 LogEntry (打序号+时间戳) 并入队到所有 sink (线程安全, 非阻塞)
    /// - [module] 产生日志的模块名 (见 [LogEntry::module]); 空 = 未指定
    void dispatch(LogLevel level, std::string_view module, std::string message);

    /// 兼容旧签名 (无模块名; 插件/外部调用方按老接口调用时不至于链接失败)
    void dispatch(LogLevel level, std::string message) {
        dispatch(level, {}, std::move(message));
    }

    /// 按模块名调整最低输出级别 (计划 OBS-5): 低于该级别的日志直接丢弃, 不入队
    /// - [modulePrefix] 模块名或前缀: 取**最长匹配**的一条生效 (相同长度时后注册者胜);
    ///   例如注册 `modelcall` 只影响该文件, 注册 `plugin.` 影响所有 `plugin.*` 模块
    /// - 未命中任何条目的模块不受影响 (照常全部入队, 由 sink 侧自行过滤级别)
    /// - 热路径开销: dispatch 内一次小表线性查找 (条目数是人为配置的个位数)
    void setModuleLevel(std::string_view modulePrefix, LogLevel minLevel);

    /// 清空全部按模块级别设置 (恢复"不过滤")
    void clearModuleLevels();

    /// 已注册的按模块级别条目数 (诊断/测试)
    size_t moduleLevelCount() const;

    /// 等待所有 sink 队列排空 (用于进程退出前确保日志不丢)
    void flush();

private:

    using SinkList = std::vector<std::weak_ptr<LogSink>>;

    /// 按模块级别设置 (copy-on-write 快照: dispatch 无锁读, 设置时复制替换)
    struct ModuleLevel {
        std::string modulePrefix;
        LogLevel    minLevel = LogLevel::Trace;
    };
    using ModuleLevelList = std::vector<ModuleLevel>;

    LogDispatcher() :
        sinks_(std::make_shared<const SinkList>()) {}

    /// 该模块是否应被丢弃 (最长前缀匹配; 未命中返回 false = 不过滤)
    bool filteredByModuleLevel(std::string_view module, LogLevel level) const;

    /// 仅用于序列化 add/remove 的 copy-on-write (注册罕见, 不在热路径)
    std::mutex mutex_;
    /// sink 快照: dispatch 无锁 load, add/remove 复制后原子 store
    AtomicSharedPtr<const SinkList> sinks_;
    /// 按模块级别快照 (dispatch 无锁 load)
    AtomicSharedPtr<const ModuleLevelList> moduleLevels_;
    /// 全局日志序号
    std::atomic<uint64_t> seq_{0};
};

/// XX_LOG 宏统一入口: 格式化后入队到所有已注册的 sink
UTILXX_BASE_API void xxLogPrint(LogLevel level, std::string message);

/// 带模块名的日志入口 (XX_LOG* 宏使用; [module] 由 [logModuleOf] 从 __FILE__ 求得)
UTILXX_BASE_API
    void xxLogPrint(LogLevel level, std::string_view module, std::string message);

/// 解析并应用"按模块日志级别"配置 (计划 OBS-5)
///
/// - 规格形如 `前缀=级别,前缀=级别`, 级别取 `trace|debug|info|warn|error` (大小写不敏感);
/// - 空段与非法段跳过并返回其数量 (调用方决定是否告警), 全部跳过时不动已有设置;
/// - 只设置本次给出的条目 (不清理之前调用的结果)
///
/// - `args`:
///     - [spec] 规格串 (通常来自环境变量 `AGENTXX_LOG_MODULES`)
///
/// - `return` 成功应用的条目数; `invalidEntries` 非空时写入被跳过的段 (便于告警)
UTILXX_BASE_API size_t applyLogModuleLevelSpec(
    std::string_view spec,
    std::vector<std::string>* invalidEntries = nullptr
);

#if XX_IS_LINUX_D

UTILXX_BASE_API void printStack();

UTILXX_BASE_API void signalError(std::string_view exepath);

#else

UTILXX_BASE_API void printStack();

UTILXX_BASE_API void signalError(std::string_view exepath);

#endif

} // namespace utilxx_base

#define XX_LOG_MODULE (::utilxx_base::logModuleOf(__FILE__))

#define XX_LOGT(str, ...)                              \
    (::utilxx_base::xxLogPrint(                      \
        ::utilxx_base::LogLevel::Trace,              \
        XX_LOG_MODULE,                               \
        fmt::format(str, ##__VA_ARGS__)              \
    ));

#define XX_LOGD(str, ...)                              \
    (::utilxx_base::xxLogPrint(                      \
        ::utilxx_base::LogLevel::Debug,              \
        XX_LOG_MODULE,                               \
        fmt::format(str, ##__VA_ARGS__)              \
    ));

#define XX_LOGI(str, ...)                                                       \
    (::utilxx_base::xxLogPrint(                                               \
        ::utilxx_base::LogLevel::Info,                                        \
        XX_LOG_MODULE,                                                        \
        fmt::format(str, ##__VA_ARGS__)                                       \
    ));

#define XX_LOGW(str, ...)                                                       \
    (::utilxx_base::xxLogPrint(                                               \
        ::utilxx_base::LogLevel::Warn,                                        \
        XX_LOG_MODULE,                                                        \
        fmt::format(str, ##__VA_ARGS__)                                       \
    ));

#define XX_LOGE(str, ...)                              \
    (::utilxx_base::xxLogPrint(                      \
        ::utilxx_base::LogLevel::Error,              \
        XX_LOG_MODULE,                               \
        fmt::format(str, ##__VA_ARGS__)              \
    ));

#define XX_OUT(str, ...)                                                       \
    (::utilxx_base::xxLogPrint(                                               \
        ::utilxx_base::LogLevel::Out,                                         \
        XX_LOG_MODULE,                                                        \
        fmt::format(str, ##__VA_ARGS__)                                       \
    ));

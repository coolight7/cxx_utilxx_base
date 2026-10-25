#pragma once

/// utilxx 版本标识与产品标识
///
/// - 版本号由构建侧经宏 `XX_VERSION_STRING` 注入 (superbuild 传入项目版本号);
///   未注入时回退 "0.1.0"
/// - 产品名默认取库名 (`cxx_utilxx`), 使用方可经 [setProductName] 改成自己的名字;
///   两者组成对外标识串 (见 [productToken]), 用于 HTTP 服务端 `Server` 头、
///   HTTP 客户端 `User-Agent` 与 WS 客户端 `User-Agent` 等
/// - 区分: 此处为软件发行版本, 与插件 ABI 契约版本 (`PLUGINXX_API_VERSION`) 无关

#ifndef XX_VERSION_STRING
#define XX_VERSION_STRING "0.1.0"
#endif

#include <string>
#include <string_view>

#include "utilxx_base/export.h"

namespace utilxx_base {

/// 项目发布版本字符串视图 (如 "0.1.0")
inline constexpr std::string_view kVersion = XX_VERSION_STRING;

/// 未设置产品名时的默认值 (库自身名字)
inline constexpr std::string_view kDefaultProductName = "cxx_utilxx";

/// 设置对外标识中的产品名 (进程级; 通常在程序启动时设置一次)
/// - 传空串或纯空白时恢复默认值 [kDefaultProductName]
/// - 线程安全; 已发出的请求不受影响, 之后构造的对外标识使用新值
UTILXX_BASE_API void setProductName(std::string_view name);

/// 当前产品名 (默认 [kDefaultProductName])
[[nodiscard]] UTILXX_BASE_API std::string productName();

/// 对外标识串: `产品名/版本号` (如 "myapp/1.0.0")
/// - 形如 RFC 9110 的 `product = token ["/" product-version]`, 可直接用作
///   `User-Agent` / `Server` 头的值
[[nodiscard]] UTILXX_BASE_API std::string productToken();

} // namespace utilxx_base

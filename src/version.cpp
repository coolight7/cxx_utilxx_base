/// 产品名与对外标识串 (见 utilxx_base/version.h)

#include "utilxx_base/version.h"

#include <mutex>
#include <string>

namespace {

/// 产品名存储 (进程级, 受互斥保护)
/// - 用函数内静态量: 避免静态初始化顺序问题, 首次使用时构造
std::string& productNameStorage() {
    static std::string storage{std::string{utilxx_base::kDefaultProductName}};
    return storage;
}

std::mutex& productNameMutex() {
    static std::mutex mutex;
    return mutex;
}

/// 全为空白视为未设置
bool isBlank(std::string_view text) {
    for (char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return false;
        }
    }
    return true;
}

} // namespace

void utilxx_base::setProductName(std::string_view name) {
    std::lock_guard<std::mutex> lock{productNameMutex()};
    if (name.empty() || isBlank(name)) {
        productNameStorage().assign(kDefaultProductName);
        return;
    }
    productNameStorage().assign(name);
}

std::string utilxx_base::productName() {
    std::lock_guard<std::mutex> lock{productNameMutex()};
    return productNameStorage();
}

std::string utilxx_base::productToken() {
    // 一次分配拼好 product = 产品名 "/" 版本号
    std::lock_guard<std::mutex> lock{productNameMutex()};
    std::string                 token = productNameStorage();
    token.push_back('/');
    token.append(kVersion);
    return token;
}

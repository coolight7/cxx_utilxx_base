#include "utilxx_base/string_util.h"
#include "boost/beast/core/detail/base64.hpp"
#include "uchardet/uchardet.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iconv.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

const utilxx_base::IgnoreCaseSet g_encoding_priorities = {
    "UTF-8",
    "UTF-16",
    "GB2312",
    "GBK",
    "GB18030",
};
const utilxx_base::IgnoreCaseMap<std::string> g_encoding_shift = {
    {"UTF8",   "UTF-8"  },
    {"UTF16",  "UTF-16" },
    {"GB2312", "GB18030"},
    {"GBK",    "GB18030"},
};
const size_t defShortStringLength = 30;

static const thread_local struct ChardetHolder {
    uchardet_t handle;

    ChardetHolder() :
        handle(uchardet_new()) {}

    ~ChardetHolder() {
        if (handle) {
            uchardet_delete(handle);
        }
    }
} g_chardetHandle;

// BOM 判断UTF16编码
static std::string detectUtfBom(std::string_view str) {
    if (str.size() < 2) {
        // UTF16 BOM至少2字节，长度不足直接返回
        return "";
    }
    const unsigned char* bom = reinterpret_cast<const unsigned char*>(str.data());
    if (bom[0] == 0xFF && bom[1] == 0xFE) {
        // UTF-16LE BOM (0xFF 0xFE)
        return "UTF-16LE";
    } else if (bom[0] == 0xFE && bom[1] == 0xFF) {
        // UTF-16BE BOM (0xFE 0xFF)
        return "UTF-16BE";
    } else if (str.size() >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        return "UTF-8";
    }
    // 无BOM，返回空
    return "";
}

static std::vector<std::string> getIconvCandidateEncodings(std::string_view src_encoding) {
    std::vector<std::string> candidates;
    if (src_encoding.empty()) {
        return candidates;
    }
    std::string enc{src_encoding};
    // UTF-16LE，Windows默认
    if (enc == "UTF-16LE" || enc == "UTF16LE" || enc == "UTF-16") {
        candidates = {"UTF-16LE", "UCS-2LE", "UTF16LE"};
    } else if (enc == "UTF-16BE" || enc == "UTF16BE") {
        // UTF-16BE
        candidates = {"UTF-16BE", "UCS-2BE", "UTF16BE"};
    } else {
        // 其他编码
        candidates.push_back(enc);
    }
    return candidates;
}

std::tuple<bool, std::optional<std::string>> utilxx_base::convertCharset(
    std::string_view src,
    std::string_view srcEncoding,
    std::string_view targetEncoding
) {
    // 已是目标编码/空字符串，直接返回
    if (utilxx_base::isIgnoreCaseEqual(srcEncoding, targetEncoding)) {
        return {true, std::nullopt};
    }
    if (src.empty()) {
        return {false, std::nullopt};
    }

    auto candidate_encs = getIconvCandidateEncodings(srcEncoding);
    if (candidate_encs.empty()) {
        return {false, std::nullopt};
    }

    // 遍历候选编码，逐个尝试
    auto target_enc = fmt::format("{}//TRANSLIT//IGNORE", utilxx_base::toUpper(targetEncoding));
    iconv_t cd      = (iconv_t)-1;
    // 记录实际使用的成功编码名
    std::string used_src_enc;
    for (const auto& enc : candidate_encs) {
        std::string src_enc = enc + "//TRANSLIT//IGNORE";
        cd                  = iconv_open(target_enc.c_str(), src_enc.c_str());
        if (cd != (iconv_t)-1) {
            used_src_enc = enc;
            break;
        }
    }
    if (cd == (iconv_t)-1) {
        // 所有候选编码名都失败，直接返回
        return {false, std::nullopt};
    }

    // 缓冲区: 初始按单字节转换为 8 字节编码预估, 不足 (E2BIG) 时自动扩容重试
    size_t      src_len    = src.size();
    size_t      dst_offset = 0;
    std::string targetStr;
    targetStr.resize(src_len * 8);

    const char* src_ptr    = src.data();
    size_t      src_remain = src_len;
    bool        ok         = false;
    while (true) {
        char*  dst_ptr    = targetStr.data() + dst_offset;
        size_t dst_remain = targetStr.size() - dst_offset;
        auto   ret = iconv(cd, const_cast<char**>(&src_ptr), &src_remain, &dst_ptr, &dst_remain);
        dst_offset = targetStr.size() - dst_remain;
        if (ret != static_cast<size_t>(-1)) {
            // 转换结束, 仅当输入完全消耗才算成功
            ok = (src_remain == 0);
            break;
        }
        if (errno == E2BIG) {
            // 输出缓冲区不足, 扩容后继续
            targetStr.resize(targetStr.size() * 2);
            continue;
        }
        // EILSEQ / EINVAL 等不可恢复错误
        break;
    }

    iconv_close(cd);
    if (!ok) {
        return {false, std::nullopt};
    }
    targetStr.resize(dst_offset);
    return {true, targetStr};
}

/// <isSuccess, result>
std::tuple<bool, std::optional<std::string>> utilxx_base::autoConvertCharset(
    std::string_view str,
    std::string&     encoding,
    std::string_view targetEncoding
) {
    if (str.empty()) {
        return {true, std::nullopt};
    }

    // 手动检测UTF16 BOM
    std::string bom_enc = detectUtfBom(str);
    if (!bom_enc.empty()) {
        encoding                 = bom_enc;
        auto [isSuccess, result] = utilxx_base::convertCharset(str, encoding, targetEncoding);
        if (isSuccess) {
            return {true, result};
        }
    }

    // uchardet检测
    uchardet_reset(g_chardetHandle.handle);
    int ret = uchardet_handle_data(g_chardetHandle.handle, str.data(), str.size());
    if (ret != 0) {
        return {false, std::nullopt};
    }
    uchardet_data_end(g_chardetHandle.handle);

    auto n_candidates = uchardet_get_n_candidates(g_chardetHandle.handle);
    if (n_candidates <= 0) {
        return {false, std::nullopt};
    }
    // if (n_candidates > 5) {
    //     n_candidates = 5;
    // }
    std::vector<std::string> detected_candidates;
    for (size_t i = 0; i < n_candidates; ++i) {
        const char* enc = uchardet_get_encoding(g_chardetHandle.handle, i);
        if (enc && std::strlen(enc) > 0) {
            detected_candidates.emplace_back(enc);
        }
    }
    if (detected_candidates.empty()) {
        return {false, std::nullopt};
    }

    std::string selected_enc;
    {
        bool haveCheckUtf8 = false;
        // detected_candidates 中可能出现多次 utf8
        for (const auto& item : detected_candidates) {
            if (g_encoding_priorities.contains(item)) {
                auto item_ptr = &item;

                auto shiftItemIt = g_encoding_shift.find(item);
                if (shiftItemIt != g_encoding_shift.end()) {
                    item_ptr = &(shiftItemIt->second);
                }

                if (str.size() < defShortStringLength) {
                    // 短字符串容易不准确，需要检查utf8有效性
                    if (*item_ptr == std::string_view{"UTF-8"}) {
                        if (haveCheckUtf8 || false == utilxx_base::utf8GetLengthCheckAvail(str)) {
                            haveCheckUtf8 = true;
                            continue;
                        }
                    }
                }
                selected_enc = *item_ptr;
                break;
            }
        }
    }
    if (selected_enc.empty()) {
        // 无优先，取第一个
        if (str.size() < defShortStringLength) {
            // 短字符串直接取 gb
            selected_enc = "GB18030";
        } else {
            selected_enc = detected_candidates[0];
        }
    }

    encoding = selected_enc;
    if (encoding == "UTF-16") {
        // 通用UTF-16, 强制转为UTF-16LE，适配Windows
        encoding = "UTF-16LE";
    }

    return utilxx_base::convertCharset(str, encoding, targetEncoding);
}

std::tuple<bool, std::optional<std::string>>
    utilxx_base::autoConvertToUtf8(std::string_view str, bool _) {
    if (str.empty()) {
        return {true, std::nullopt};
    }
    std::string encoding;
    return autoConvertCharset(str, encoding, "UTF-8");
}

bool utilxx_base::autoConvertToUtf8(std::string& str) {
    if (str.empty()) {
        return true;
    }
    auto [isSuccess, result] = autoConvertToUtf8(std::string_view{str}, true);
    if (isSuccess && result.has_value()) {
        str = std::move(result.value());
    }
    return isSuccess;
}

std::string utilxx_base::autoTryConvertToUtf8(std::string_view str) {
    auto [isSuccess, result] = autoConvertToUtf8(str, true);
    if (isSuccess && result.has_value()) {
        return std::move(result.value());
    }
    return std::string{str};
}

std::u16string utilxx_base::utf8ToUtf16(std::string_view str) {
    std::u16string result{};
    result.reserve(str.size());
    const auto* data = reinterpret_cast<const unsigned char*>(str.data());
    const auto  size = str.size();
    size_t      i    = 0;
    while (i < size) {
        const unsigned char first = data[i];
        if (first < 0x80) {
            result.push_back(static_cast<char16_t>(first));
            ++i;
            continue;
        }
        // 各长度序列的后续字节数
        int      extra     = 0;
        char32_t codePoint = 0;
        char32_t minValue  = 0;
        if ((first & 0xE0) == 0xC0) {
            extra     = 1;
            codePoint = first & 0x1F;
            minValue  = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            extra     = 2;
            codePoint = first & 0x0F;
            minValue  = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            extra     = 3;
            codePoint = first & 0x07;
            minValue  = 0x10000;
        } else {
            result.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= size) {
            result.push_back(0xFFFD);
            break;
        }
        bool isOk = true;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char next = data[i + k];
            if ((next & 0xC0) != 0x80) {
                isOk = false;
                break;
            }
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        if (false == isOk || codePoint < minValue || codePoint > 0x10FFFF
            || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            // 非法序列或非最短编码：只吃掉一个字节，后面的继续按正常流程解
            result.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += static_cast<size_t>(extra) + 1;
        if (codePoint <= 0xFFFF) {
            result.push_back(static_cast<char16_t>(codePoint));
        } else {
            const char32_t value = codePoint - 0x10000;
            result.push_back(static_cast<char16_t>(0xD800 + (value >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00 + (value & 0x3FF)));
        }
    }
    return result;
}

std::string utilxx_base::utf16ToUtf8(std::u16string_view str) {
    std::string result{};
    result.reserve(str.size());
    for (size_t i = 0; i < str.size(); ++i) {
        char32_t codePoint = str[i];
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && i + 1 < str.size()) {
            const char16_t next = str[i + 1];
            if (next >= 0xDC00 && next <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (next - 0xDC00);
                ++i;
            } else {
                codePoint = 0xFFFD;
            }
        } else if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
            codePoint = 0xFFFD;
        }
        if (codePoint < 0x80) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    return result;
}

std::string utilxx_base::base64Encode(std::string_view data) {
    if (data.empty()) {
        // 空输入合法, 解码为空结果
        return std::string{};
    }

    // 预分配
    std::string result;
    result.resize(boost::beast::detail::base64::encoded_size(data.size()));

    size_t bytes_written
        = boost::beast::detail::base64::encode(result.data(), data.data(), data.size());

    // 调整大小为实际写入的字节数
    result.resize(bytes_written);
    return result;
}

std::string utilxx_base::base64Encode(const void* data, size_t size) {
    if (nullptr == data || 0 == size) {
        return std::string{};
    }
    return base64Encode(std::string_view{static_cast<const char*>(data), size});
}

std::optional<std::string> utilxx_base::base64Decode(std::string_view str) {
    if (str.empty()) {
        // 空输入合法, 解码为空结果
        return std::string{};
    }

    {
        size_t padCount = 0;
        // 校验 base64 格式: 仅含合法字符, '=' 仅在结尾 (最多 2 个), 数据长度 mod 4 不为 1
        for (size_t i = 0; i < str.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(str[i]);
            if (c == '=') {
                // '=' 只能出现在结尾最多 2 个; 先判 str.size() >= 2 避免 size_t 下溢
                if (str.size() < 2 || i + 2 < str.size()) {
                    return std::nullopt;
                }
                ++padCount;
            } else {
                bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                             || (c >= '0' && c <= '9') || c == '+' || c == '/';
                if (!valid) {
                    return std::nullopt;
                }
                if (padCount > 0) {
                    return std::nullopt; // padding 之后不应再出现数据字符
                }
            }
        }
        if ((str.size() - padCount) % 4 == 1) {
            return std::nullopt; // 非法 base64 长度
        }
    }

    std::string result;
    // decoded_size(n) 要求 n 为 4 的倍数, 对未补齐 padding 的输入 (长度 mod4 ∈ {2,3})
    // 会分配不足导致越界写; 故先向上取整到 4 的倍数再计算最大解码长度
    result.resize(boost::beast::detail::base64::decoded_size((str.size() + 3) / 4 * 4));

    auto [bytes_written, chars_read]
        = boost::beast::detail::base64::decode(result.data(), str.data(), str.size());

    // [decode] 会忽略末尾的 =, 因此可能出现 `chars_read < str.size()`
    if (chars_read == 0) {
        return std::nullopt;
    }
    result.resize(bytes_written);
    return result;
}

std::string utilxx_base::getFirstWordPinyin(std::string_view str) {
    if (s_pinyinCallback) {
        return s_pinyinCallback(str);
    }
    return "";
}

std::string utilxx_base::getFirstCharPinyinFast(std::string_view str) {
    if (str.empty()) {
        return "";
    }

    int code = static_cast<unsigned char>(str[0]);
    if (isCode_num(code)) {
        return std::string(1, str[0]);
    }
    if (isCode_az(code)) {
        return std::string(1, str[0]);
    }
    if (isCode_AZ(code)) {
        return std::string(1, static_cast<char>(code + (CODE_a - CODE_A)));
    }

    std::string result = getFirstWordPinyin(str);
    if (!result.empty()) {
        int firstCode = static_cast<unsigned char>(result[0]);
        if (isCode_AZaz(firstCode)) {
            toLowerSelf(result);
            return result;
        }
    }
    return "";
}

std::optional<int> utilxx_base::getComparableCode(std::string_view str, size_t index) {
    int code = static_cast<unsigned char>(str[index]);

    if (isCode_az(code)) {
        return code;
    }
    if (isCode_AZ(code)) {
        return code + (CODE_a - CODE_A);
    }

    if (code < 128) {
        return std::nullopt;
    }

    auto        target = (index == 0) ? str : str.substr(index);
    std::string pinyin = getFirstCharPinyinFast(target);
    if (!pinyin.empty()) {
        int pinyinCode = static_cast<unsigned char>(pinyin[0]);
        if (isCode_AZ(pinyinCode)) {
            return pinyinCode + (CODE_a - CODE_A);
        }
        if (isCode_az(pinyinCode)) {
            return pinyinCode;
        }
    }
    return std::nullopt;
}

std::optional<std::string>
    utilxx_base::getFirstCharPinyin(std::string_view str, bool enableAZ, bool enableNum) {
    if (str.empty()) {
        return std::nullopt;
    }

    std::string trimmed = removeBetweenSpace(str, true, true, false);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    int code = static_cast<unsigned char>(trimmed[0]);

    if (isCode_num(code)) {
        if (enableNum) {
            return std::string(1, trimmed[0]);
        } else {
            return std::nullopt;
        }
    } else if (isCode_az(code)) {
        if (enableAZ) {
            return std::string(1, trimmed[0]);
        } else {
            return std::nullopt;
        }
    } else if (isCode_AZ(code)) {
        if (enableAZ) {
            return std::string(1, static_cast<char>(code + (CODE_a - CODE_A)));
        } else {
            return std::nullopt;
        }
    } else {
        std::string result = getFirstWordPinyin(trimmed);
        if (!result.empty()) {
            int firstCode = static_cast<unsigned char>(result[0]);
            if (isCode_AZaz(firstCode)) {
                toLowerSelf(result);
                return result;
            }
        }
        return std::nullopt;
    }
}

std::optional<std::string> utilxx_base::getFirstCharPinyinFirstChar(std::string_view str) {
    auto restr = getFirstCharPinyin(str);
    if (restr.has_value() && !restr->empty()) {
        return std::string(1, (*restr)[0]);
    }
    return std::nullopt;
}

int utilxx_base::compareExtend(std::string_view left, std::string_view right) {
    if (left.empty()) {
        if (right.empty()) {
            return 0;
        }
        return -1;
    }
    if (right.empty()) {
        return 1;
    }

    size_t i = 0, j = 0;

    while (i < left.size() && j < right.size()) {
        int  leftCode   = static_cast<unsigned char>(left[i]);
        int  rightCode  = static_cast<unsigned char>(right[j]);
        bool leftIsNum  = isCode_num(leftCode);
        bool rightIsNum = isCode_num(rightCode);

        if (leftIsNum != rightIsNum) {
            return leftIsNum ? -1 : 1;
        }

        if (leftIsNum) {
            // 用 int64 累加数字段, 避免原 int 累加在超长数字串(如 file99999999999)下溢出(UB);
            // 保持返回"数值差"的原有语义(测试依赖精确差值), 超出 int 范围时饱和截断(符号仍正确)。
            constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
            int64_t           leftSum   = 0;
            while (i < left.size() && isCode_num(static_cast<unsigned char>(left[i]))) {
                const int d = static_cast<unsigned char>(left[i]) - CODE_0;
                // 用 (kInt64Max - d) / 10 判定: leftSum*10+d 必然 <= kInt64Max,
                // 避免在边界附近 (如 922337203685477580 + d>=8) 有符号溢出 (UB)
                leftSum = (leftSum > (kInt64Max - d) / 10) ? kInt64Max : (leftSum * 10 + d);
                i++;
            }
            int64_t rightSum = 0;
            while (j < right.size() && isCode_num(static_cast<unsigned char>(right[j]))) {
                const int d = static_cast<unsigned char>(right[j]) - CODE_0;
                rightSum    = (rightSum > (kInt64Max - d) / 10) ? kInt64Max : (rightSum * 10 + d);
                j++;
            }
            if (leftSum != rightSum) {
                const int64_t diff = leftSum - rightSum;
                if (diff > std::numeric_limits<int>::max()) {
                    return std::numeric_limits<int>::max();
                }
                if (diff < std::numeric_limits<int>::min()) {
                    return std::numeric_limits<int>::min();
                }
                return static_cast<int>(diff);
            }
            continue;
        }

        auto leftComp  = getComparableCode(left, i);
        auto rightComp = getComparableCode(right, j);

        if (leftComp.has_value() && rightComp.has_value()) {
            int result = leftComp.value() - rightComp.value();
            if (result != 0) {
                return result;
            }
        } else if (leftComp.has_value()) {
            return 1;
        } else if (rightComp.has_value()) {
            return -1;
        } else {
            int leftLower  = toCode_tryaz(leftCode).value_or(leftCode);
            int rightLower = toCode_tryaz(rightCode).value_or(rightCode);
            int result     = leftLower - rightLower;
            if (result != 0) {
                return result;
            }
        }

        i++;
        j++;
    }

    if (i < left.size() || j < right.size()) {
        // 保持返回"总长度差"的原有语义; 经 int64 相减避免 size_t 下溢(right 更长时), 超 int
        // 范围饱和截断
        const int64_t diff = static_cast<int64_t>(left.size()) - static_cast<int64_t>(right.size());
        if (diff > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        if (diff < std::numeric_limits<int>::min()) {
            return std::numeric_limits<int>::min();
        }
        return static_cast<int>(diff);
    }
    return 0;
}

std::string utilxx_base::replaceOrAppendExt(std::string_view inpath, std::string_view newExt) {
    auto ext = getFileNameEXT(inpath);
    if (ext.has_value()) {
        auto result = std::string{inpath};
        result.replace(inpath.size() - ext->size(), ext->size(), newExt);
        return result;
    }
    if (!inpath.empty() && inpath.back() == '.') {
        return fmt::format("{}{}", inpath, newExt);
    }
    return fmt::format("{}.{}", inpath, newExt);
}

std::string utilxx_base::toArgument(std::string_view str, char mark) {
    std::string result;
    result.reserve(str.size() * 2);
    size_t len = str.size();

    for (size_t i = 0; i < len; ++i) {
        if (str[i] == mark) {
            // 统计 mark 前连续反斜杠数量: 偶数个 (含 0) 时 mark 是新的界定符, 需转义;
            // 奇数个时 mark 已被反斜杠转义 (如 \" 表示字面引号), 不应再转义
            // (仅看紧邻前一个字符会在 "\\\"" 偶数个反斜杠时漏转义, 导致引号提前闭合)
            size_t backslashes = 0;
            while (i > backslashes && str[i - backslashes - 1] == '\\') {
                ++backslashes;
            }
            if (backslashes % 2 == 0) {
                result += '\\';
            }
            result += mark;
        } else {
            result += str[i];
        }
    }

    return fmt::format("{}{}{}", mark, result, mark);
}

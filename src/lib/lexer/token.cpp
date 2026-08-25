#include "lexer/token.h"

#include "util/hashmap.h"

namespace khu::lexer {
namespace {

struct KindInfo {
    const char* name;
    const char* spelling;  // empty for kinds without a fixed spelling
};

const KindInfo* kind_table() {
    static KindInfo table[static_cast<std::size_t>(TokenKind::Count)] = {};
    static bool initialized = false;
    if (initialized) return table;

    for (auto& entry : table) entry = KindInfo{"<unknown>", ""};
#define KHU_SPECIAL_ENTRY(name, description) \
    table[static_cast<std::size_t>(TokenKind::name)] = KindInfo{description, ""};
    KHU_TOKEN_SPECIALS(KHU_SPECIAL_ENTRY)
#undef KHU_SPECIAL_ENTRY

#define KHU_FIXED_ENTRY(name, spelling) \
    table[static_cast<std::size_t>(TokenKind::name)] = KindInfo{spelling, spelling};
    KHU_TOKEN_KEYWORDS(KHU_FIXED_ENTRY)
    KHU_TOKEN_PUNCTUATION(KHU_FIXED_ENTRY)
#undef KHU_FIXED_ENTRY

    table[static_cast<std::size_t>(TokenKind::KeywordFirst)] = KindInfo{"<keywords>", ""};
    table[static_cast<std::size_t>(TokenKind::KeywordLast)] = KindInfo{"<keywords>", ""};

    initialized = true;
    return table;
}

const khu::util::StringMap<TokenKind>& keyword_table() {
    static khu::util::StringMap<TokenKind> map = [] {
        khu::util::StringMap<TokenKind> table;
#define KHU_KEYWORD_ENTRY(name, spelling) table.insert(spelling, TokenKind::name);
        KHU_TOKEN_KEYWORDS(KHU_KEYWORD_ENTRY)
#undef KHU_KEYWORD_ENTRY
        return table;
    }();
    return map;
}

}  // namespace

const char* token_name(TokenKind kind) {
    if (kind >= TokenKind::Count) return "<unknown>";
    return kind_table()[static_cast<std::size_t>(kind)].name;
}

std::string_view token_spelling(TokenKind kind) {
    if (kind >= TokenKind::Count) return {};
    return kind_table()[static_cast<std::size_t>(kind)].spelling;
}

bool is_keyword(TokenKind kind) {
    return kind > TokenKind::KeywordFirst && kind < TokenKind::KeywordLast;
}

bool is_type_keyword(TokenKind kind) {
    switch (kind) {
#define KHU_TYPE_ENTRY(name, spelling) case TokenKind::name:
        KHU_TOKEN_TYPE_KEYWORDS(KHU_TYPE_ENTRY)
#undef KHU_TYPE_ENTRY
        return true;
        default:
            return false;
    }
}

TokenKind keyword_kind(std::string_view text) {
    const TokenKind* found = keyword_table().find(text);
    return found ? *found : TokenKind::Identifier;
}

}  // namespace khu::lexer

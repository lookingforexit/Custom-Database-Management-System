#include "parser/lexer.hpp"

#include <regex>

// this file implements tokenization for the sql-like input language.
namespace dbms::parser {

    std::vector<Token> Lexer::Tokenize(const std::string &sql) {
        std::vector<Token> tokens;

        // Match multi-character operators first, then identifiers/literals, then single-char punctuation.
        std::regex token_regex(
            R"(\s*(==|!=|<=|>=|'([^']|'')*'|\b\w+\b|[(),.;*+\-/<>=!])\s*)");

        auto words_begin = std::sregex_iterator(sql.begin(), sql.end(), token_regex);
        auto words_end = std::sregex_iterator();

        for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
            const std::smatch& match = *i;
            std::string token = match[1].str();
            if (!token.empty()) {
                tokens.push_back(Token{token});
            }
        }

        return tokens;
    }

} // namespace dbms::parser

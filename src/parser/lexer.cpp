#include "parser/lexer.hpp"

#include <cctype>

// this file implements tokenization for the sql-like input language.
namespace dbms::parser {

    std::vector<Token> Lexer::Tokenize(const std::string &sql) {
        std::vector<Token> tokens;
        std::size_t i = 0;
        while (i < sql.size()) {
            const char ch = sql[i];
            if (std::isspace(static_cast<unsigned char>(ch))) {
                ++i;
                continue;
            }

            if (i + 1 < sql.size()) {
                const std::string two = sql.substr(i, 2);
                if (two == "==" || two == "!=" || two == "<=" || two == ">=") {
                    tokens.push_back(Token{two});
                    i += 2;
                    continue;
                }
            }

            if (ch == '"') {
                std::string literal;
                literal.push_back(ch);
                ++i;
                while (i < sql.size()) {
                    literal.push_back(sql[i]);
                    if (sql[i] == '"' && literal.size() > 1 &&
                        literal[literal.size() - 2] != '\\') {
                        ++i;
                        break;
                    }
                    ++i;
                }
                tokens.push_back(Token{literal});
                continue;
            }

            if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') {
                std::size_t start = i;
                while (i < sql.size() &&
                       (std::isalnum(static_cast<unsigned char>(sql[i])) ||
                        sql[i] == '_')) {
                    ++i;
                }
                tokens.push_back(Token{sql.substr(start, i - start)});
                continue;
            }

            if (ch == ',' || ch == '(' || ch == ')' || ch == '.' || ch == ';' ||
                ch == '*' || ch == '<' || ch == '>' || ch == '=') {
                tokens.push_back(Token{std::string(1, ch)});
                ++i;
                continue;
            }

            tokens.push_back(Token{std::string(1, ch)});
            ++i;
        }

        return tokens;
    }

} // namespace dbms::parser

#pragma once

// this file declares tokenization for the sql-like input language.
#include <string>
#include <vector>

namespace dbms::parser {

struct Token {
    std::string lexeme;
};

class Lexer {
public:
    [[nodiscard]] std::vector<Token> Tokenize(const std::string& sql) const;
};

}  // namespace dbms::parser

#pragma once

// this file defines tokenization interfaces for the sql-like input language.
#include <string>
#include <vector>

namespace dbms::parser {

    struct Token {
        std::string lexeme;
    };

    class Lexer {
      public:
        static std::vector<Token> Tokenize(const std::string &sql) ;
    };

} // namespace dbms::parser

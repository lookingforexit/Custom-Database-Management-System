#include "parser/lexer.hpp"

// this file will split query text into tokens with proper sql rules.
namespace dbms::parser {

std::vector<Token> Lexer::Tokenize(const std::string& sql) const {
    return {{sql}};
}

}  // namespace dbms::parser

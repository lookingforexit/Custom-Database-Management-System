#include "parser/lexer.hpp"

// this file implements tokenization for the sql-like input language.
namespace dbms::parser {

  std::vector<Token> Lexer::Tokenize(const std::string &sql) const {
    return {{sql}};
  }

} // namespace dbms::parser

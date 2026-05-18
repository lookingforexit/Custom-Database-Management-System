#pragma once

// this file defines the parser interface that builds ast nodes from sql text.
#include <string>

#include "common/result.hpp"
#include "parser/ast.hpp"

namespace dbms::parser {

  class Parser {
  public:
    [[nodiscard]] common::Result<Statement> Parse(const std::string &sql) const;
  };

} // namespace dbms::parser

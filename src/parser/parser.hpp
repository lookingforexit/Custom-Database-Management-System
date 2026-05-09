#pragma once

// this file declares the parser that builds ast nodes from tokens.
#include <string>

#include "common/result.hpp"
#include "parser/ast.hpp"

namespace dbms::parser {

class Parser {
public:
    [[nodiscard]] common::Result<Statement> Parse(const std::string& sql) const;
};

}  // namespace dbms::parser

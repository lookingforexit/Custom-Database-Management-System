#include "parser/parser.hpp"

#include <cctype>
#include <stdexcept>

#include "common/error.hpp"
#include "common/result.hpp"
#include "parser/lexer.hpp"

// this file implements sql parsing for ddl, dml, filters, and aggregates.
namespace dbms::parser {

    namespace {

        std::string ToUpper(std::string s) {
            for (char &ch : s) {
                ch = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(ch)));
            }
            return s;
        }

        bool IsIdentifier(const std::string &token) {
            if (token.empty()) {
                return false;
            }
            if (!std::isalpha(static_cast<unsigned char>(token[0])) &&
                token[0] != '_') {
                return false;
            }
            for (char ch : token) {
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
                    return false;
                }
            }
            return true;
        }

        class Cursor {
          public:
            explicit Cursor(std::vector<Token> tokens)
                : tokens_(std::move(tokens)) {}

            [[nodiscard]] bool Has() const { return pos_ < tokens_.size(); }
            [[nodiscard]] const std::string &Peek() const {
                static const std::string kEmpty;
                return Has() ? tokens_[pos_].lexeme : kEmpty;
            }
            [[nodiscard]] const std::string &PeekN(std::size_t offset) const {
                static const std::string kEmpty;
                const std::size_t index = pos_ + offset;
                return index < tokens_.size() ? tokens_[index].lexeme : kEmpty;
            }
            std::string Take() {
                if (!Has()) {
                    throw std::runtime_error("unexpected end of input");
                }
                return tokens_[pos_++].lexeme;
            }
            bool MatchKeyword(const std::string &kw) {
                if (!Has()) {
                    return false;
                }
                if (ToUpper(Peek()) != kw) {
                    return false;
                }
                ++pos_;
                return true;
            }
            bool Match(const std::string &token) {
                if (!Has() || Peek() != token) {
                    return false;
                }
                ++pos_;
                return true;
            }
            void Expect(const std::string &token) {
                if (!Match(token)) {
                    throw std::runtime_error("expected token: " + token);
                }
            }
            void ExpectKeyword(const std::string &kw) {
                if (!MatchKeyword(kw)) {
                    throw std::runtime_error("expected keyword: " + kw);
                }
            }
            std::string TakeIdentifier() {
                const std::string token = Take();
                if (!IsIdentifier(token)) {
                    throw std::runtime_error("invalid identifier: " + token);
                }
                return token;
            }

          private:
            std::vector<Token> tokens_;
            std::size_t pos_{0};
        };

        common::Value ParseLiteral(const std::string &token) {
            if (token == "NULL" || token == "null") {
                return std::monostate{};
            }
            if (!token.empty() && token.front() == '"' && token.back() == '"' &&
                token.size() >= 2) {
                return token.substr(1, token.size() - 2);
            }

            std::size_t idx = 0;
            const long long v = std::stoll(token, &idx);
            if (idx != token.size()) {
                throw std::runtime_error("invalid literal: " + token);
            }
            return static_cast<std::int64_t>(v);
        }

        QualifiedName ParseQualifiedName(Cursor &c) {
            const std::string first = c.TakeIdentifier();
            if (c.Match(".")) {
                const std::string second = c.TakeIdentifier();
                return QualifiedName{.database_name = first, .object_name = second};
            }
            return QualifiedName{.database_name = std::nullopt, .object_name = first};
        }

        Expression ParseExpression(Cursor &c);

        Expression ParsePrimary(Cursor &c) {
            if (c.Match("(")) {
                Expression inner = ParseExpression(c);
                c.Expect(")");
                return inner;
            }
            if (IsIdentifier(c.Peek())) {
                auto q = ParseQualifiedName(c);
                return Expression{ColumnReferenceExpression{
                    .database_name = q.database_name,
                    .table_name = std::nullopt,
                    .column_name = q.object_name,
                }};
            }
            return Expression{LiteralExpression{.value = ParseLiteral(c.Take())}};
        }

        ComparisonOperator ParseCmpOp(const std::string &token) {
            if (token == "==") return ComparisonOperator::kEqual;
            if (token == "!=") return ComparisonOperator::kNotEqual;
            if (token == "<") return ComparisonOperator::kLess;
            if (token == ">") return ComparisonOperator::kGreater;
            if (token == "<=") return ComparisonOperator::kLessEqual;
            if (token == ">=") return ComparisonOperator::kGreaterEqual;
            throw std::runtime_error("unknown comparison operator: " + token);
        }

        Expression ParsePredicate(Cursor &c) {
            Expression left = ParsePrimary(c);

            if (c.MatchKeyword("BETWEEN")) {
                Expression lower = ParsePrimary(c);
                c.ExpectKeyword("AND");
                Expression upper = ParsePrimary(c);
                return Expression{BetweenExpression{
                    .value = std::make_unique<Expression>(std::move(left)),
                    .lower_bound = std::make_unique<Expression>(std::move(lower)),
                    .upper_bound = std::make_unique<Expression>(std::move(upper)),
                }};
            }
            if (c.MatchKeyword("LIKE")) {
                Expression pattern = ParsePrimary(c);
                return Expression{LikeExpression{
                    .value = std::make_unique<Expression>(std::move(left)),
                    .pattern = std::make_unique<Expression>(std::move(pattern)),
                }};
            }

            const std::string op = c.Take();
            Expression right = ParsePrimary(c);
            return Expression{BinaryComparisonExpression{
                .op = ParseCmpOp(op),
                .left = std::make_unique<Expression>(std::move(left)),
                .right = std::make_unique<Expression>(std::move(right)),
            }};
        }

        Expression ParseAnd(Cursor &c) {
            Expression left = ParsePredicate(c);
            while (c.MatchKeyword("AND")) {
                Expression right = ParsePredicate(c);
                left = Expression{LogicalExpression{
                    .op = LogicalOperator::kAnd,
                    .left = std::make_unique<Expression>(std::move(left)),
                    .right = std::make_unique<Expression>(std::move(right)),
                }};
            }
            return left;
        }

        Expression ParseExpression(Cursor &c) {
            Expression left = ParseAnd(c);
            while (c.MatchKeyword("OR")) {
                Expression right = ParseAnd(c);
                left = Expression{LogicalExpression{
                    .op = LogicalOperator::kOr,
                    .left = std::make_unique<Expression>(std::move(left)),
                    .right = std::make_unique<Expression>(std::move(right)),
                }};
            }
            return left;
        }

        Statement ParseCreate(Cursor &c) {
            if (c.MatchKeyword("DATABASE")) {
                return CreateDatabaseStatement{.database_name = c.TakeIdentifier()};
            }
            c.ExpectKeyword("TABLE");
            CreateTableStatement stmt;
            stmt.table_name = ParseQualifiedName(c);
            c.Expect("(");
            while (true) {
                ColumnDefinition col;
                col.name = c.TakeIdentifier();
                const std::string type = ToUpper(c.Take());
                if (type == "INT") {
                    col.type = common::ValueType::kInt64;
                } else if (type == "STRING") {
                    col.type = common::ValueType::kString;
                } else {
                    throw std::runtime_error("unsupported column type: " + type);
                }

                while (true) {
                    if (c.MatchKeyword("NOT_NULL")) {
                        col.not_null = true;
                        continue;
                    }
                    if (c.MatchKeyword("INDEXED")) {
                        col.indexed = true;
                        col.not_null = true;
                        continue;
                    }
                    if (c.MatchKeyword("DEFAULT")) {
                        col.default_value = ParseLiteral(c.Take());
                        continue;
                    }
                    break;
                }
                stmt.columns.push_back(std::move(col));
                if (c.Match(")")) {
                    break;
                }
                c.Expect(",");
            }
            return stmt;
        }

        Statement ParseDrop(Cursor &c) {
            if (c.MatchKeyword("DATABASE")) {
                return DropDatabaseStatement{.database_name = c.TakeIdentifier()};
            }
            c.ExpectKeyword("TABLE");
            return DropTableStatement{.table_name = ParseQualifiedName(c)};
        }

        Statement ParseUse(Cursor &c) {
            return UseDatabaseStatement{.database_name = c.TakeIdentifier()};
        }

        Statement ParseInsert(Cursor &c) {
            c.ExpectKeyword("INTO");
            InsertStatement stmt;
            stmt.table_name = ParseQualifiedName(c);
            c.Expect("(");
            while (true) {
                stmt.column_names.push_back(c.TakeIdentifier());
                if (c.Match(")")) {
                    break;
                }
                c.Expect(",");
            }
            if (!c.MatchKeyword("VALUE")) {
                c.ExpectKeyword("VALUES");
            }
            do {
                c.Expect("(");
                std::vector<Expression> row;
                while (true) {
                    row.push_back(
                        Expression{LiteralExpression{.value = ParseLiteral(c.Take())}});
                    if (c.Match(")")) {
                        break;
                    }
                    c.Expect(",");
                }
                stmt.rows.push_back(std::move(row));
            } while (c.Match(","));
            return stmt;
        }

        Statement ParseUpdate(Cursor &c) {
            UpdateStatement stmt;
            stmt.table_name = ParseQualifiedName(c);
            c.ExpectKeyword("SET");
            while (true) {
                Assignment a;
                a.column_name = c.TakeIdentifier();
                c.Expect("=");
                a.value =
                    Expression{LiteralExpression{.value = ParseLiteral(c.Take())}};
                stmt.assignments.push_back(std::move(a));
                if (!c.Match(",")) {
                    break;
                }
            }
            if (c.MatchKeyword("WHERE")) {
                stmt.where = ParseExpression(c);
            }
            return stmt;
        }

        Statement ParseDelete(Cursor &c) {
            c.ExpectKeyword("FROM");
            DeleteStatement stmt;
            stmt.table_name = ParseQualifiedName(c);
            if (c.MatchKeyword("WHERE")) {
                stmt.where = ParseExpression(c);
            }
            return stmt;
        }

        std::optional<AggregateKind> ParseAgg(const std::string &token) {
            const std::string t = ToUpper(token);
            if (t == "SUM") return AggregateKind::kSum;
            if (t == "COUNT") return AggregateKind::kCount;
            if (t == "AVG") return AggregateKind::kAvg;
            return std::nullopt;
        }

        Statement ParseSelect(Cursor &c) {
            SelectStatement stmt;
            if (c.Match("*")) {
                stmt.items.push_back(SelectItem{.is_wildcard = true});
            } else {
                while (true) {
                    SelectItem item;
                    if (auto agg = ParseAgg(c.Peek()); agg.has_value()) {
                        c.Take();
                        c.Expect("(");
                        item.column_name = c.TakeIdentifier();
                        c.Expect(")");
                        item.aggregate = agg;
                    } else {
                        item.column_name = c.TakeIdentifier();
                    }
                    if (c.MatchKeyword("AS")) {
                        item.alias = c.TakeIdentifier();
                    }
                    stmt.items.push_back(std::move(item));
                    if (!c.Match(",")) {
                        break;
                    }
                }
            }
            c.ExpectKeyword("FROM");
            stmt.table_name = ParseQualifiedName(c);
            if (c.MatchKeyword("WHERE")) {
                stmt.where = ParseExpression(c);
            }
            return stmt;
        }

        Statement ParseRevert(Cursor &c) {
            RevertStatement stmt;
            stmt.table_name = ParseQualifiedName(c);
            stmt.timestamp = c.Take();
            return stmt;
        }

    } // namespace

    common::Result<Statement> Parser::Parse(const std::string &sql) const {
        try {
            Cursor c(Lexer::Tokenize(sql));
            if (!c.Has()) {
                return common::MakeError<Statement>(common::ErrorCode::kParseError,
                                                    "empty SQL");
            }

            Statement stmt;
            if (c.MatchKeyword("CREATE")) {
                stmt = ParseCreate(c);
            } else if (c.MatchKeyword("DROP")) {
                stmt = ParseDrop(c);
            } else if (c.MatchKeyword("USE")) {
                stmt = ParseUse(c);
            } else if (c.MatchKeyword("INSERT")) {
                stmt = ParseInsert(c);
            } else if (c.MatchKeyword("UPDATE")) {
                stmt = ParseUpdate(c);
            } else if (c.MatchKeyword("DELETE")) {
                stmt = ParseDelete(c);
            } else if (c.MatchKeyword("SELECT")) {
                stmt = ParseSelect(c);
            } else if (c.MatchKeyword("REVERT")) {
                stmt = ParseRevert(c);
            } else {
                return common::MakeError<Statement>(
                    common::ErrorCode::kParseError, "unsupported statement");
            }

            if (c.Match(";")) {
                if (c.Has()) {
                    return common::MakeError<Statement>(
                        common::ErrorCode::kParseError,
                        "unexpected tokens after semicolon");
                }
                return common::MakeSuccess(std::move(stmt));
            }
            if (c.Has()) {
                return common::MakeError<Statement>(
                    common::ErrorCode::kParseError,
                    "unexpected trailing tokens");
            }
            return common::MakeSuccess(std::move(stmt));
        } catch (const std::exception &e) {
            return common::MakeError<Statement>(common::ErrorCode::kParseError,
                                                e.what());
        }
    }

} // namespace dbms::parser

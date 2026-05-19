#include "parser/parser.hpp"

#include <cctype>
#include <stdexcept>
#include <unordered_set>

#include "common/error.hpp"
#include "common/result.hpp"
#include "common/types.hpp"
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

            const std::string next_upper = ToUpper(c.Peek());
            if (!c.Has() || c.Peek() == ")" || c.Peek() == ";" ||
                next_upper == "AND" || next_upper == "OR") {
                return left;
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
                a.value = ParsePrimary(c);
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
                        if (c.Match("*")) {
                            item.column_name = "*";
                        } else {
                            item.column_name = c.TakeIdentifier();
                        }
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
            if (c.MatchKeyword("LATEST")) {
                stmt.mode = RevertStatement::RevertMode::kLatest;
                stmt.timestamp = "LATEST";
                return stmt;
            }
            if (c.MatchKeyword("EXACT")) {
                stmt.mode = RevertStatement::RevertMode::kExact;
            } else if (c.MatchKeyword("AT_OR_BEFORE")) {
                stmt.mode = RevertStatement::RevertMode::kAtOrBefore;
            }
            stmt.timestamp = c.Take();
            if (stmt.timestamp.size() >= 2 && stmt.timestamp.front() == '"' &&
                stmt.timestamp.back() == '"') {
                stmt.timestamp =
                    stmt.timestamp.substr(1, stmt.timestamp.size() - 2);
            }
            return stmt;
        }

        Statement ParseBegin(Cursor &c) {
            if (c.MatchKeyword("TRANSACTION")) {
                return BeginTransactionStatement{};
            }
            return BeginTransactionStatement{};
        }

        Statement ParseCommit(Cursor &c) {
            if (c.MatchKeyword("TRANSACTION")) {
                return CommitTransactionStatement{};
            }
            return CommitTransactionStatement{};
        }

        Statement ParseRollback(Cursor &c) {
            if (c.MatchKeyword("TRANSACTION")) {
                return RollbackTransactionStatement{};
            }
            return RollbackTransactionStatement{};
        }

        void ValidateCreateTable(const CreateTableStatement &stmt) {
            if (stmt.columns.empty()) {
                throw std::runtime_error("CREATE TABLE must define columns");
            }

            std::unordered_set<std::string> names;
            for (const auto &column : stmt.columns) {
                if (!names.insert(column.name).second) {
                    throw std::runtime_error("duplicate column name: " +
                                             column.name);
                }
                if (column.default_value.has_value()) {
                    if (!common::CanAssignToType(*column.default_value,
                                                 column.type, !column.not_null)) {
                        throw std::runtime_error(
                            "DEFAULT value type mismatch for column: " +
                            column.name);
                    }
                }
            }
        }

        void ValidateInsert(const InsertStatement &stmt) {
            if (stmt.column_names.empty()) {
                throw std::runtime_error(
                    "INSERT must specify at least one target column");
            }
            if (stmt.rows.empty()) {
                throw std::runtime_error("INSERT must include at least one row");
            }

            std::unordered_set<std::string> names;
            for (const auto &name : stmt.column_names) {
                if (!names.insert(name).second) {
                    throw std::runtime_error("duplicate INSERT column: " + name);
                }
            }

            for (const auto &row : stmt.rows) {
                if (row.size() != stmt.column_names.size()) {
                    throw std::runtime_error(
                        "INSERT row value count does not match column count");
                }
            }
        }

        void ValidateUpdate(const UpdateStatement &stmt) {
            if (stmt.assignments.empty()) {
                throw std::runtime_error("UPDATE must define assignments");
            }

            std::unordered_set<std::string> names;
            for (const auto &assignment : stmt.assignments) {
                if (!names.insert(assignment.column_name).second) {
                    throw std::runtime_error("duplicate UPDATE assignment: " +
                                             assignment.column_name);
                }
            }
        }

        void ValidateSelect(const SelectStatement &stmt) {
            if (stmt.items.empty()) {
                throw std::runtime_error("SELECT item list is empty");
            }
            bool has_aggregate = false;
            bool has_non_aggregate = false;
            if (stmt.items.size() > 1) {
                for (const auto &item : stmt.items) {
                    if (item.is_wildcard) {
                        throw std::runtime_error(
                            "SELECT * cannot be combined with other items");
                    }
                    if (item.aggregate.has_value()) {
                        has_aggregate = true;
                    } else {
                        has_non_aggregate = true;
                    }
                }
            }
            if (stmt.items.size() == 1) {
                has_aggregate = stmt.items[0].aggregate.has_value();
                has_non_aggregate = !has_aggregate && !stmt.items[0].is_wildcard;
            }
            if (has_aggregate && has_non_aggregate) {
                throw std::runtime_error(
                    "cannot mix aggregate and non-aggregate select items");
            }
            for (const auto &item : stmt.items) {
                if (!item.aggregate.has_value()) {
                    continue;
                }
                if (item.column_name == "*" &&
                    item.aggregate.value() != AggregateKind::kCount) {
                    throw std::runtime_error(
                        "only COUNT supports wildcard argument '*'");
                }
            }
        }

        void ValidateStatement(const Statement &stmt) {
            if (const auto *create_table =
                    std::get_if<CreateTableStatement>(&stmt);
                create_table != nullptr) {
                ValidateCreateTable(*create_table);
                return;
            }
            if (const auto *insert = std::get_if<InsertStatement>(&stmt);
                insert != nullptr) {
                ValidateInsert(*insert);
                return;
            }
            if (const auto *update = std::get_if<UpdateStatement>(&stmt);
                update != nullptr) {
                ValidateUpdate(*update);
                return;
            }
            if (const auto *select = std::get_if<SelectStatement>(&stmt);
                select != nullptr) {
                ValidateSelect(*select);
                return;
            }
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
            } else if (c.MatchKeyword("BEGIN")) {
                stmt = ParseBegin(c);
            } else if (c.MatchKeyword("COMMIT")) {
                stmt = ParseCommit(c);
            } else if (c.MatchKeyword("ROLLBACK")) {
                stmt = ParseRollback(c);
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
                ValidateStatement(stmt);
                return common::MakeSuccess(std::move(stmt));
            }
            if (c.Has()) {
                return common::MakeError<Statement>(
                    common::ErrorCode::kParseError,
                    "unexpected trailing tokens");
            }
            ValidateStatement(stmt);
            return common::MakeSuccess(std::move(stmt));
        } catch (const std::exception &e) {
            return common::MakeError<Statement>(common::ErrorCode::kParseError,
                                                e.what());
        }
    }

} // namespace dbms::parser

#include "interpreter.hpp"
#include "MiniLua/environment.hpp"
#include "MiniLua/exceptions.hpp"
#include "MiniLua/interpreter.hpp"
#include "MiniLua/io.hpp"
#include "MiniLua/metatables.hpp"
#include "MiniLua/stdlib.hpp"
#include "ast.hpp"
#include "tree_sitter/tree_sitter.hpp"

#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

// This points to a binary blob containing the lua/stdlib.lua file.
// The file is put into the compiled object by the linker.
extern const char _binary_stdlib_lua_start[]; // NOLINT
extern const char _binary_stdlib_lua_end[];   // NOLINT

namespace minilua::details {

// TODO remove the unimplemented stuff once everything is implemented
class UnimplementedException : public InterpreterException {
public:
    UnimplementedException(const std::string& where, const std::string& what)
        : InterpreterException("unimplemented: \"" + what + "\" in " + where) {}
};

#define UNIMPLEMENTED(what)                                                                        \
    UnimplementedException(                                                                        \
        std::string(__func__) + " (" + std::string(__FILE__) + ":" + std::to_string(__LINE__) +    \
            ")",                                                                                   \
        what)

// struct EvalResult
EvalResult::EvalResult() : values(), do_break(false), do_return(false) {}
EvalResult::EvalResult(Vallist values)
    : values(std::move(values)), do_break(false), do_return(false) {}
EvalResult::EvalResult(const CallResult& call_result)
    : values(call_result.values()), do_break(false), do_return(false),
      source_change(call_result.source_change()) {}

void EvalResult::combine(const EvalResult& other) {
    this->values = other.values;
    this->do_break = other.do_break;
    this->do_return = other.do_return;
    this->source_change = combine_source_changes(this->source_change, other.source_change);
}

EvalResult::operator minilua::EvalResult() const {
    minilua::EvalResult result;
    result.value = this->values.get(0);
    result.source_change = this->source_change;
    return result;
}

auto operator<<(std::ostream& o, const EvalResult& self) -> std::ostream& {
    o << "EvalResult{ "
      << ".value = " << self.values << ", .do_break = " << self.do_break
      << ", .do_return = " << self.do_return << ", .source_change = ";

    if (self.source_change.has_value()) {
        o << *self.source_change;
    } else {
        o << "nullopt";
    }

    return o << "}";
}

// class Interpreter
Interpreter::Interpreter(const InterpreterConfig& config, ts::Parser& parser)
    : config(config), parser(parser) {}

auto Interpreter::run(const ts::Tree& tree, Env& user_env) -> EvalResult {
    Env env = this->setup_environment(user_env);

    // execute the actual program
    std::shared_ptr<std::string> root_filename = std::make_shared<std::string>("__root__");
    env.set_file(root_filename);

    try {
        auto result = this->run_file(tree, env);

        if (result.values.size() != 0) {
            // don't return functions (or tables that contains functions)
            // this prevents crashes because it is not allwed to call lua functions
            // after the interpreter finishes
            //
            // also ignore everything except the first value because the public api
            // only returns one value
            if (result.values.get(0).contains_function()) {
                result.values = Vallist();
            } else {
                result.values = Vallist(result.values.get(0));
            }
        }

        // NOTE: This will not free the tables it only calls all __gc metamethods.
        this->cleanup_environment(env);

        return result;
    } catch (const std::runtime_error& e) {
        this->cleanup_environment(env);
        throw;
    } catch (const InterpreterException& e) {
        this->cleanup_environment(env);
        throw;
    }
}

auto Interpreter::setup_environment(Env& user_env) -> Env {
    Env env(user_env.allocator());

    // load the C++ part of the stdlib
    add_stdlib(env.global());
    // run the Lua part of the stdlib
    this->execute_stdlib(env);

    // apply user overwrites
    // NOTE we only consider global variables because the user can only set
    // global variables
    env.global().set_all(user_env.global());

    // copy over relevant information from user_env
    env.set_file(user_env.get_file());
    env.set_stdin(user_env.get_stdin());
    env.set_stdout(user_env.get_stdout());
    env.set_stderr(user_env.get_stderr());

    // set a default filename for the root file if the user did not set one
    if (!env.get_file().has_value()) {
        std::shared_ptr<std::string> root_filename = std::make_shared<std::string>("__root__");
        env.set_file(root_filename);
    }

    return env;
}

void Interpreter::execute_stdlib(Env& env) {
    // NOTE The tree is static so it is only initialized once
    static const ts::Tree stdlib_tree = this->load_stdlib();

    try {
        env.set_file(std::nullopt);
        this->run_file(stdlib_tree, env);
    } catch (const std::exception& e) {
        // This should never actually throw an exception
        throw InterpreterException(
            std::string("THIS IS A BUG! Failed to execute the stdlib file: ") + e.what());
    }
}

auto Interpreter::load_stdlib() -> ts::Tree {
    // NOTE this method should only be called once when the stdlib_tree in
    // Interpreter::execute_stdlib is initialized

    // load the Lua part of the stdlib
    // NOTE the result of executing the stdlib file will be ignored

    std::string stdlib_code(_binary_stdlib_lua_start, _binary_stdlib_lua_end);

    try {
        ts::Tree stdlib_tree = this->parser.parse_string(std::move(stdlib_code));

        // This is just in case. Failing to parse is a bug!!!
        if (stdlib_tree.root_node().has_error()) {
            std::stringstream ss;
            ts::visit_tree(stdlib_tree, [&ss](ts::Node node) {
                if (node.type() == std::string("ERROR") || node.is_missing()) {
                    ss << "Error in node: ";
                    ss << ts::debug_print_node(node);
                }
            });
            throw std::runtime_error(ss.str());
        }

        return stdlib_tree;
    } catch (const std::exception& e) {
        // This should never actually throw an exception
        throw InterpreterException(
            std::string("THIS IS A BUG! Failed to parse the stdlib: ") + e.what());
    }
}

void Interpreter::cleanup_environment(Env& env) {
    for (auto* table_impl : env.allocator()->get_all()) {
        Environment environment(env);
        CallContext ctx(&environment);

        Table table(table_impl, env.allocator());

        mt::gc(ctx.make_new({table}));
    }
}

auto Interpreter::run_file(const ts::Tree& tree, Env& env) -> EvalResult {
    try {
        return this->visit_root(ast::Program(tree.root_node()), env);
    } catch (const InterpreterException&) {
        throw;
    } catch (const std::exception& e) {
        throw InterpreterException(std::string("unknown error: ") + e.what());
    }
}

auto Interpreter::tracer() const -> std::ostream& { return *this->config.target; }
void Interpreter::trace_enter_node(
    std::string ast_class, std::optional<std::string> method_name) const {
    if (this->config.trace_nodes) {
        this->tracer() << "Enter node: " << ast_class;
        if (method_name) {
            this->tracer() << " (method: " << method_name.value() << ")";
        }
        this->tracer() << "\n";
    }
}
void Interpreter::trace_exit_node(
    std::string ast_class, std::optional<std::string> method_name,
    std::optional<std::string> reason) const {
    if (this->config.trace_nodes) {
        this->tracer() << "Exit node: " << ast_class;
        if (method_name) {
            this->tracer() << " (method: " << method_name.value() << ")";
        }
        if (reason) {
            this->tracer() << " reason: " << reason.value();
        }
        this->tracer() << "\n";
    }
}
void Interpreter::trace_function_call(
    ast::Prefix prefix, const std::vector<Value>& arguments) const {
    auto function_name = std::string(prefix.to_string());
    if (this->config.trace_calls) {
        this->tracer() << "Calling function: " << function_name << " with arguments (";
        for (const auto& arg : arguments) {
            this->tracer() << arg << ", ";
        }
        this->tracer() << ")\n";
    }
}
void Interpreter::trace_function_call_result(ast::Prefix prefix, const CallResult& result) const {
    if (this->config.trace_calls) {
        auto function_name = std::string(prefix.to_string());
        this->tracer() << "Function call to: " << function_name << " resulted in "
                       << result.values();
        if (result.source_change().has_value()) {
            this->tracer() << " with source changes " << result.source_change().value();
        }
        this->tracer() << "\n";
    }
}
void Interpreter::trace_metamethod_call(const std::string& name, const Vallist& arguments) const {
    if (this->config.trace_metamethod_calls) {
        this->tracer() << "Calling Metamethod: " << name << " with arguments (";
        for (const auto& arg : arguments) {
            this->tracer() << arg << ", ";
        }
        this->tracer() << ")\n";
    }
}
void Interpreter::trace_exprlists(
    std::vector<ast::Expression>& exprlist, const Vallist& result) const {
    if (this->config.trace_exprlists) {
        this->tracer() << "Exprlist: (";
        const auto* sep = "";
        for (auto& expr : exprlist) {
            this->tracer() << sep << expr.to_string();
            sep = ", ";
        }
        this->tracer() << ") resulted in (";

        sep = "";
        for (const auto& value : result) {
            this->tracer() << sep << value;
            sep = ", ";
        }
        this->tracer() << ")\n";
    }
}
void Interpreter::trace_enter_block(Env& env) {
    if (this->config.trace_enter_block) {
        this->tracer() << "Enter block: " << env << "\n";
    }
}

// class Interpreter::NodeTracer
Interpreter::NodeTracer::NodeTracer(
    Interpreter* interpreter, std::string ast_class, std::optional<std::string> method_name)
    : interpreter(*interpreter), ast_class(ast_class), method_name(std::move(method_name)) {
    this->interpreter.trace_enter_node(this->ast_class, this->method_name);
}
Interpreter::NodeTracer::~NodeTracer() {
    this->interpreter.trace_exit_node(this->ast_class, this->method_name);
}

// helper functions
static auto convert_range(ts::Range range) -> Range {
    return Range{
        .start = {range.start.point.row, range.start.point.column, range.start.byte},
        .end = {range.end.point.row, range.end.point.column, range.end.byte},
    };
}

// interpreter implementation
auto Interpreter::visit_root(ast::Program program, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, program.debug_print(), "visit_root");

    EvalResult result;

    auto body = program.body();

    for (auto child : body.statements()) {
        EvalResult sub_result = this->visit_statement(child, env);
        result.combine(sub_result);
    }

    if (body.return_statement()) {
        result.combine(this->visit_return_statement(body.return_statement().value(), env));
    }

    return result;
}

auto Interpreter::visit_statement(ast::Statement statement, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, statement.debug_print(), "visit_statement");

    auto result = std::visit(
        overloaded{
            [this, &env](ast::VariableDeclaration node) {
                return this->visit_variable_declaration(node, env);
            },
            [this, &env](ast::DoStatement node) { return this->visit_do_statement(node, env); },
            [this, &env](ast::IfStatement node) { return this->visit_if_statement(node, env); },
            [this, &env](ast::WhileStatement node) {
                return this->visit_while_statement(node, env);
            },
            [this, &env](ast::RepeatStatement node) {
                return this->visit_repeat_until_statement(node, env);
            },
            [this, &env](ast::ForStatement node) -> EvalResult {
                return this->visit_do_statement(node.desugar(), env);
            },
            [this, &env](ast::ForInStatement node) -> EvalResult {
                return this->visit_do_statement(node.desugar(), env);
            },
            [this, &env](ast::GoTo node) -> EvalResult { throw UNIMPLEMENTED("goto"); },
            [this, &env](ast::Break node) { return this->visit_break_statement(); },
            [this, &env](ast::Label node) -> EvalResult { throw UNIMPLEMENTED("label"); },
            [this, &env](ast::FunctionStatement node) {
                return this->visit_variable_declaration(node.desugar(), env);
            },
            [this, &env](ast::FunctionCall node) -> EvalResult {
                return this->visit_function_call(node, env);
            },
            [this, &env](ast::Expression node) { return this->visit_expression(node, env); },
        },
        statement.options());

    if (!result.do_return) {
        result.values = Vallist();
    }

    return result;
}

auto Interpreter::visit_do_statement(ast::DoStatement do_stmt, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, do_stmt.debug_print(), "visit_do_statement");

    return this->visit_block(do_stmt.body(), env);
}

auto Interpreter::visit_block(ast::Body block, Env& env) -> EvalResult {
    Env block_env = Env(env);
    return this->visit_block_with_local_env(std::move(block), block_env);
}
auto Interpreter::visit_block_with_local_env(ast::Body block, Env& block_env) -> EvalResult {
    this->trace_enter_block(block_env);

    EvalResult result;

    for (auto stmt : block.statements()) {
        auto sub_result = this->visit_statement(stmt, block_env);
        result.combine(sub_result);

        if (result.do_break) {
            return result;
        }
        if (result.do_return) {
            return result;
        }
    }

    auto return_stmt = block.return_statement();
    if (return_stmt) {
        auto sub_result = this->visit_return_statement(return_stmt.value(), block_env);
        result.combine(sub_result);
    }

    return result;
}

auto Interpreter::visit_if_statement(ast::IfStatement if_stmt, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, if_stmt.debug_print(), "visit_if_statement");

    EvalResult result;

    // NOTE: scope is here so I can reuse variable names without problems
    {
        // if condition condition
        auto condition = if_stmt.condition();
        auto condition_result = this->visit_expression(condition, env);
        result.combine(condition_result);

        // "then" block
        if (condition_result.values.get(0)) {
            auto body_result = this->visit_block(if_stmt.body(), env);
            result.combine(body_result);

            return result;
        }
    }

    for (auto elseif_stmt : if_stmt.elseifs()) {
        // else if condition
        auto condition = elseif_stmt.condition();
        auto condition_result = this->visit_expression(condition, env);
        result.combine(condition_result);

        // "else if" block
        if (condition_result.values.get(0)) {
            auto body_result = this->visit_block(elseif_stmt.body(), env);
            result.combine(body_result);

            return result;
        }
    }

    // "else" block
    auto else_stmt = if_stmt.else_statement();
    if (else_stmt) {
        auto body_result = this->visit_block(else_stmt->body(), env);
        result.combine(body_result);
    }

    return result;
}

auto Interpreter::visit_while_statement(ast::WhileStatement while_stmt, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, while_stmt.debug_print(), "visit_while_statement");

    EvalResult result;

    auto condition = while_stmt.repeat_conditon();

    while (true) {
        auto condition_result = this->visit_expression(condition, env);
        result.combine(condition_result);

        // repeat while condition is true
        if (!condition_result.values.get(0)) {
            return result;
        }

        auto block_result = this->visit_block(while_stmt.body(), env);
        result.combine(block_result);

        if (result.do_break) {
            result.do_break = false;
            return result;
        }
        if (result.do_return) {
            return result;
        }
    }

    return result;
}

auto Interpreter::visit_repeat_until_statement(ast::RepeatStatement repeat_stmt, Env& env)
    -> EvalResult {
    auto _ = NodeTracer(this, repeat_stmt.debug_print(), "visit_repeat_until_statement");

    EvalResult result;

    auto body = repeat_stmt.body();
    auto condition = repeat_stmt.repeat_condition();

    while (true) {
        Env block_env = Env(env);

        auto block_result = this->visit_block_with_local_env(body, block_env);
        result.combine(block_result);

        if (result.do_break) {
            result.do_break = false;
            return result;
        }
        if (result.do_return) {
            return result;
        }

        // the condition is part of the same block and can access local variables
        // declared in the repeat block
        auto condition_result = this->visit_expression(condition, block_env);
        result.combine(condition_result);

        // repeat until condition is true
        if (condition_result.values.get(0)) {
            return result;
        }
    }

    return result;
}

auto Interpreter::visit_break_statement() -> EvalResult {
    if (this->config.trace_break) {
        this->tracer() << "break\n";
    }

    EvalResult result;
    result.do_break = true;
    return result;
}

auto Interpreter::visit_expression_list(std::vector<ast::Expression> expressions, Env& env)
    -> EvalResult {
    EvalResult result;
    std::vector<Value> return_values;

    if (!expressions.empty()) {
        for (int i = 0; i < expressions.size() - 1; ++i) {
            const auto expr = expressions[i];
            const auto sub_result = this->visit_expression(expr, env);
            result.combine(sub_result);
            return_values.push_back(sub_result.values.get(0));
        }

        // if the last element has a vallist (i.e. because it was a function call) the vallist is
        // appended
        auto expr = expressions[expressions.size() - 1];
        const auto sub_result = this->visit_expression(expr, env);
        result.combine(sub_result);
        return_values.insert(
            return_values.end(), sub_result.values.begin(), sub_result.values.end());
    }

    result.values = return_values;

    this->trace_exprlists(expressions, result.values);

    return result;
}

auto Interpreter::visit_return_statement(ast::Return return_stmt, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, return_stmt.debug_print(), "visit_return_statement");

    auto result = this->visit_expression_list(return_stmt.exp_list(), env);
    result.do_return = true;

    return result;
}

auto Interpreter::visit_variable_declaration(ast::VariableDeclaration decl, Env& env)
    -> EvalResult {
    auto _ = NodeTracer(this, decl.debug_print(), "visit_variable_declaration");

    EvalResult result = this->visit_expression_list(decl.declarations(), env);
    const auto vallist = result.values;

    auto targets = decl.declarators();

    for (int i = 0; i < decl.declarators().size(); ++i) {
        auto target = targets[i].options();
        const auto& value = vallist.get(i);

        if (decl.local()) {
            // the only target that is allowed for local declarations is an identifier
            std::visit(
                overloaded{
                    [this, &env, &value](ast::Identifier ident) {
                        auto ident_str = this->visit_identifier(ident, env);
                        env.declare_local(ident_str);
                        env.set_local(ident_str, value);
                    },
                    [](ast::FieldExpression /*node*/) {
                        throw InterpreterException(
                            "Field expression not allowed as target of local declaration");
                    },
                    [](ast::TableIndex node) {
                        throw InterpreterException(
                            "Table access not allowed as target of local declaration");
                    },
                },
                target);
        } else {
            std::visit(
                overloaded{
                    [this, &env, &value](ast::Identifier ident) {
                        env.set_var(this->visit_identifier(ident, env), value);
                    },
                    [this, &env, &value, &result](ast::TableIndex table_index) {
                        // evaluate the prefix (i.e. the part before the square brackets)
                        auto prefix_result = this->visit_prefix(table_index.table(), env);
                        result.combine(prefix_result);
                        auto table = prefix_result.values.get(0);

                        // evaluate the index (i.e. the part inside the square brackets)
                        auto index_result = this->visit_expression(table_index.index(), env);
                        result.combine(index_result);
                        auto index = index_result.values.get(0);

                        Environment environment(env);
                        CallContext ctx(&environment);

                        auto newindex_call_result =
                            mt::newindex(ctx.make_new({table, index, value}));
                        result.combine(EvalResult(newindex_call_result));
                    },
                    [this, &env, &value, &result](ast::FieldExpression field_expr) {
                        // evaluate the prefix (i.e. the part before the dot)
                        auto prefix_result = this->visit_prefix(field_expr.table_id(), env);
                        result.combine(prefix_result);
                        auto table = prefix_result.values.get(0);

                        // get the property identifier (i.e. the part after the dot)
                        auto index = field_expr.property_id().string();

                        Environment environment(env);
                        CallContext ctx(&environment);

                        auto newindex_call_result =
                            mt::newindex(ctx.make_new({table, index, value}));
                        result.combine(EvalResult(newindex_call_result));
                    }},
                target);
        }
    }

    return result;
}

auto Interpreter::visit_identifier(ast::Identifier ident, Env& env) -> std::string {
    auto _ = NodeTracer(this, ident.debug_print(), "visit_identifier");
    return ident.string();
}

auto Interpreter::visit_expression(ast::Expression expr, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, expr.debug_print(), "visit_expression");

    EvalResult result = std::visit(
        overloaded{
            [this, &env](ast::Spread /*unused*/) -> EvalResult {
                return this->visit_vararg_expression(env);
            },
            [this, &env](ast::Prefix prefix) { return this->visit_prefix(prefix, env); },
            [this, &env](ast::FunctionDefinition function_definition) {
                return this->visit_function_expression(function_definition, env);
            },
            [this, &env](ast::Table table) { return this->visit_table_constructor(table, env); },
            [this, &env](ast::BinaryOperation binary_op) {
                return this->visit_binary_operation(binary_op, env);
            },
            [this, &env](ast::UnaryOperation unary_op) {
                return this->visit_unary_operation(unary_op, env);
            },
            [this, &env](ast::Literal literal) {
                return this->visit_literal(std::move(literal), env);
            },
            [this, &env](ast::Identifier ident) {
                auto variable_name = this->visit_identifier(ident, env);
                return EvalResult(Vallist(env.get_var(variable_name)));
            },
        },
        expr.options());

    return result;
}

auto Interpreter::visit_literal(ast::Literal literal, Env& env) -> EvalResult {
    EvalResult result;

    Value value;
    switch (literal.type()) {
    case ast::LiteralType::TRUE:
        value = Value(Bool(true));
        break;
    case ast::LiteralType::FALSE:
        value = Value(Bool(false));
        break;
    case ast::LiteralType::NIL:
        value = Value(Nil());
        break;
    case ast::LiteralType::NUMBER:
        value = parse_number_literal(literal.content());
        break;
    case ast::LiteralType::STRING:
        value = parse_string_literal(literal.content());
        break;
    }

    auto origin = LiteralOrigin{.location = literal.range()};
    origin.location.file = env.get_file();
    result.values = Vallist(value.with_origin(origin));

    return result;
}

auto Interpreter::visit_vararg_expression(Env& env) -> EvalResult {
    auto varargs = env.get_varargs();

    if (!varargs.has_value()) {
        throw InterpreterException("cannot use '...' outside a vararg function");
    }

    if (this->config.trace_varargs) {
        this->tracer() << "varargs: " << *varargs << "\n";
    }

    return EvalResult(varargs.value());
}

auto FunctionImpl::operator()(const CallContext& ctx) -> CallResult {
    // setup parameters as local variables
    auto env = Env(this->env);
    for (int i = 0; i < parameters.size(); ++i) {
        env.set_local(parameters[i], ctx.arguments().get(i));
    }

    // add varargs to the environment
    if (vararg) {
        std::vector<Value> varargs;
        if (parameters.size() < ctx.arguments().size()) {
            varargs.reserve(ctx.arguments().size() - parameters.size());
            std::copy(
                ctx.arguments().begin() + parameters.size(), ctx.arguments().end(),
                std::back_inserter(varargs));
        }
        env.set_varargs(varargs);
    } else {
        // explicitly unset varargs because it is only allowed to use the
        // expression `...` directly inside the vararg function and not
        // in nested functions
        env.set_varargs(std::nullopt);
    }

    // execute the actual function in the correct environment
    auto result = interpreter.visit_block_with_local_env(body, env);

    auto return_value = Vallist();
    if (result.do_return) {
        return_value = result.values;
    }
    return CallResult(return_value, result.source_change);
}

auto Interpreter::visit_parameter_list(std::vector<ast::Identifier> raw_params, Env& env)
    -> std::vector<std::string> {
    std::vector<std::string> actual_parameters;
    actual_parameters.reserve(raw_params.size());
    std::transform(
        raw_params.begin(), raw_params.end(), std::back_inserter(actual_parameters),
        [this, &env](auto ident) { return this->visit_identifier(ident, env); });

    return actual_parameters;
}

auto Interpreter::visit_function_expression(ast::FunctionDefinition function_definition, Env& env)
    -> EvalResult {
    auto _ = NodeTracer(this, function_definition.debug_print(), "visit_function_expression");

    EvalResult result;

    auto parameters = function_definition.parameters();

    std::vector<std::string> actual_parameters =
        this->visit_parameter_list(parameters.params(), env);

    bool vararg = parameters.spread();

    auto body = function_definition.body();

    Value func = Function(FunctionImpl{
        .body = std::move(body),
        .env = Env(env),
        .parameters = std::move(actual_parameters),
        .vararg = vararg,
        .interpreter = *this,
    });

    result.values = Vallist(func);

    return result;
}

auto Interpreter::visit_table_index(ast::TableIndex table_index, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, table_index.debug_print(), "visit_table_index");

    EvalResult result;

    // evaluate the prefix (i.e. the part before the square brackets)
    auto prefix_result = this->visit_prefix(table_index.table(), env);
    result.combine(prefix_result);
    auto table = prefix_result.values.get(0);

    // evaluate the index (i.e. the part inside the square brackets)
    auto index_result = this->visit_expression(table_index.index(), env);
    result.combine(index_result);
    auto index = index_result.values.get(0);

    Environment environment(env);
    CallContext ctx(&environment);

    auto index_call_result = mt::index(ctx.make_new({table, index}));
    result.combine(EvalResult(index_call_result));

    return result;
}

auto Interpreter::visit_field_expression(ast::FieldExpression field_expression, Env& env)
    -> EvalResult {
    auto _ = NodeTracer(this, field_expression.debug_print(), "visit_field_expression");

    EvalResult result;

    // evaluate the prefix (i.e. the part before the dot)
    auto table_result = this->visit_prefix(field_expression.table_id(), env);
    result.combine(table_result);
    auto table = table_result.values.get(0);

    std::string key = this->visit_identifier(field_expression.property_id(), env);

    Environment environment(env);
    CallContext ctx(&environment);

    auto index_call_result = mt::index(ctx.make_new({table, key}));
    result.combine(EvalResult(index_call_result));

    return result;
}

auto Interpreter::visit_table_constructor(ast::Table table_constructor, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, table_constructor.debug_print(), "visit_table_constructor");

    EvalResult result;

    Table table(env.allocator());

    const auto fields = table_constructor.fields();
    if (!fields.empty()) {
        // TODO move the consecutive_key logic to table because it is not completely correct
        int consecutive_key = 1;

        for (int i = 0; i < fields.size() - 1; ++i) {
            auto field = fields[i];
            auto [key, value] = std::visit(
                overloaded{
                    [this, &env, &result](std::pair<ast::Expression, ast::Expression> field)
                        -> std::pair<Value, Value> {
                        auto key_result = this->visit_expression(field.first, env);
                        result.combine(key_result);

                        auto value_result = this->visit_expression(field.second, env);
                        result.combine(value_result);

                        return std::make_pair(key_result.values.get(0), value_result.values.get(0));
                    },
                    [this, &env, &result](std::pair<ast::Identifier, ast::Expression> field)
                        -> std::pair<Value, Value> {
                        auto key = this->visit_identifier(field.first, env);

                        auto value_result = this->visit_expression(field.second, env);
                        result.combine(value_result);

                        return std::make_pair(key, value_result.values.get(0));
                    },
                    [this, &env, &result,
                     &consecutive_key](ast::Expression item) -> std::pair<Value, Value> {
                        auto item_result = this->visit_expression(item, env);
                        result.combine(item_result);
                        auto key = consecutive_key;
                        consecutive_key++;
                        return std::make_pair(key, item_result.values.get(0));
                    },
                },
                field.content());

            table.set(key, value);
        }

        // if last entry is an expression and returns a vallist the vallist is appended
        auto field = fields[fields.size() - 1];
        std::visit(
            overloaded{
                [this, &env, &result, &table](std::pair<ast::Expression, ast::Expression> field) {
                    auto key_result = this->visit_expression(field.first, env);
                    result.combine(key_result);

                    auto value_result = this->visit_expression(field.second, env);
                    result.combine(value_result);

                    table.set(key_result.values.get(0), value_result.values.get(0));
                },
                [this, &env, &result, &table](std::pair<ast::Identifier, ast::Expression> field) {
                    auto key = this->visit_identifier(field.first, env);

                    auto value_result = this->visit_expression(field.second, env);
                    result.combine(value_result);

                    table.set(key, value_result.values.get(0));
                },
                [this, &env, &result, &consecutive_key, &table](ast::Expression item) {
                    auto item_result = this->visit_expression(item, env);
                    result.combine(item_result);

                    for (const auto& value : item_result.values) {
                        auto key = consecutive_key;
                        consecutive_key++;
                        table.set(key, value);
                    }
                },
            },
            field.content());
    }

    result.values = Vallist(table);

    return result;
}

auto Interpreter::visit_binary_operation(ast::BinaryOperation bin_op, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, bin_op.debug_print(), "visit_binary_operation");

    EvalResult result;

    auto origin = bin_op.range();
    origin.file = env.get_file();

    auto lhs_result = this->visit_expression(bin_op.left(), env);
    auto lhs = lhs_result.values.get(0);
    result.combine(lhs_result);

    auto rhs_result = this->visit_expression(bin_op.right(), env);
    auto rhs = rhs_result.values.get(0);
    result.combine(rhs_result);

    // raw operators
    auto impl_operator = [&result, &origin](auto f, Value lhs, Value rhs) {
        Value value = std::invoke(f, lhs, rhs, origin);
        result.values = Vallist(value);
    };

#define IMPL(op, method)                                                                           \
    case ast::BinOpEnum::op:                                                                       \
        impl_operator(&Value::method, lhs, rhs);                                                   \
        break;

    Environment environment(env);
    CallContext ctx(&environment);

    // operators supporting metamethods
    auto impl_mt_operator = [this, &ctx, &result,
                             &origin](auto f, Value lhs, Value rhs, const std::string& name) {
        auto args = Vallist{std::move(lhs), std::move(rhs)};
        this->trace_metamethod_call(name, args);

        auto call_result = with_call_stack(
            [&f, &ctx, &args, &origin]() { return f(ctx.make_new(args), origin); }, name,
            StackItem{
                .position = origin,
                .info = "metamethod '" + name + "'",
            });
        result.combine(EvalResult(call_result.one_value()));
    };

#define IMPL_MT(op, function, name)                                                                \
    case ast::BinOpEnum::op:                                                                       \
        impl_mt_operator(function, lhs, rhs, name);                                                \
        break;

    switch (bin_op.binary_operator()) {
        // operators with metamethods

        // arithmetic
        IMPL_MT(ADD, mt::add, "add")
        IMPL_MT(SUB, mt::sub, "sub")
        IMPL_MT(MUL, mt::mul, "mul")
        IMPL_MT(DIV, mt::div, "div")
        IMPL_MT(MOD, mt::mod, "mod")
        IMPL_MT(POW, mt::pow, "pow")
        IMPL_MT(INT_DIV, mt::idiv, "idiv")

        // bitwise
        IMPL_MT(BIT_AND, mt::band, "band")
        IMPL_MT(BIT_OR, mt::bor, "bor")
        IMPL_MT(BIT_XOR, mt::bxor, "bxor")
        IMPL_MT(SHIFT_LEFT, mt::shl, "shl")
        IMPL_MT(SHIFT_RIGHT, mt::shr, "shr")
        IMPL_MT(CONCAT, mt::concat, "concat")

        // comparison
        IMPL_MT(EQ, mt::eq, "eq")
        IMPL_MT(LT, mt::lt, "lt")
        IMPL_MT(LEQ, mt::le, "le")

        // the following operators have to be converted to other metamethods
    case ast::BinOpEnum::GT:
        // gt: "x > y" == "y < x"
        impl_mt_operator(mt::lt, rhs, lhs, "gt");
        break;
    case ast::BinOpEnum::GEQ:
        // geq: "x >= y" == "y <= x"
        impl_mt_operator(mt::le, rhs, lhs, "geq");
        break;
    case ast::BinOpEnum::NEQ:
        // neq: "x ~= y" == "not (x == y)"
        impl_mt_operator(mt::eq, lhs, rhs, "eq");
        result.values = Vallist(result.values.get(0).invert());
        break;

        // these don't have metamethods
        // TODO do short circuiting
        IMPL(OR, logic_or)
        IMPL(AND, logic_and)
    }

#undef IMPL
#undef IMPL_MT

    return result;
}

auto Interpreter::visit_unary_operation(ast::UnaryOperation unary_op, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, unary_op.debug_print(), "visit_unary_operation");

    EvalResult result = this->visit_expression(unary_op.expression(), env);

    auto range = unary_op.range();
    range.file = env.get_file();

    Environment environment(env);
    CallContext ctx(&environment);

    auto impl_mt_operator = [this, &ctx, &result, &range](auto f, const std::string& name) {
        auto args = Vallist{result.values.get(0)};
        this->trace_metamethod_call(name, args);

        auto call_result = with_call_stack(
            [&f, &ctx, &args, &range]() { return f(ctx.make_new(args), range); }, name,
            StackItem{
                .position = range,
                .info = "metamethod '" + name + "'",
            });
        result.combine(EvalResult(call_result.one_value()));
    };

#define IMPL_MT(op, method, name)                                                                  \
    case ast::UnOpEnum::op:                                                                        \
        impl_mt_operator(method, name);                                                            \
        break;

    switch (unary_op.unary_operator()) {
        IMPL_MT(NEG, mt::unm, "unm")
        IMPL_MT(BWNOT, mt::bnot, "bnot")
        IMPL_MT(LEN, mt::len, "len")

    case ast::UnOpEnum::NOT:
        result.values = Vallist(result.values.get(0).invert(range));
        break;
    }

#undef IMPL
#undef IMPL_MT

    return result;
}

auto Interpreter::visit_prefix(ast::Prefix prefix, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, prefix.debug_print(), "visit_prefix");

    EvalResult result = std::visit(
        overloaded{
            [](ast::Self) -> EvalResult { throw UNIMPLEMENTED("self"); },
            [this, &env](ast::VariableDeclarator variable_decl) {
                return std::visit(
                    overloaded{
                        [this, &env](ast::Identifier ident) {
                            EvalResult result;
                            result.values =
                                Vallist(env.get_var(this->visit_identifier(ident, env)));
                            return result;
                        },
                        [this, &env](ast::FieldExpression field) {
                            // TODO desugar to table index
                            return this->visit_field_expression(field, env);
                        },
                        [this, &env](ast::TableIndex table_index) -> EvalResult {
                            return this->visit_table_index(table_index, env);
                        }},
                    variable_decl.options());
            },
            [this, &env](ast::FunctionCall call) { return this->visit_function_call(call, env); },
            [this, &env](ast::Expression expr) { return this->visit_expression(expr, env); }},
        prefix.options());

    return result;
}

static auto prefix_to_ident(const ast::Prefix& prefix) -> std::string { return prefix.to_string(); }

auto Interpreter::visit_function_call(ast::FunctionCall call, Env& env) -> EvalResult {
    auto _ = NodeTracer(this, call.debug_print(), "visit_function_call");

    EvalResult result;

    auto function_obj_result = this->visit_prefix(call.id(), env);
    result.combine(function_obj_result);

    EvalResult exprlist_result = this->visit_expression_list(call.args(), env);
    auto arguments = exprlist_result.values;

    this->trace_function_call(call.id(), std::vector<Value>(arguments.begin(), arguments.end()));

    // call function
    // this will produce an error if the obj is not callable
    auto obj = function_obj_result.values.get(0);

    // metamethod __call gets the called object as first argument
    std::vector<Value> meta_arguments;
    meta_arguments.reserve(arguments.size() + 1);
    meta_arguments.push_back(obj);
    std::move(arguments.begin(), arguments.end(), std::back_inserter(meta_arguments));

    auto call_range = call.range().with_file(env.get_file());

    // move the Env to the CallContext (and move it back later)
    auto environment = Environment(env);
    auto ctx = CallContext(&environment).make_new(meta_arguments, call_range);

    auto function_name = call.id().to_string();

    auto call_result = with_call_stack(
        [&ctx]() { return mt::call(ctx); }, function_name,
        StackItem{
            .position = call_range,
            .info = "function '" + function_name + "'",
        });
    result.combine(EvalResult(call_result));

    this->trace_function_call_result(call.id(), call_result);

    // move the Env back in case something has changed internally
    env = environment.get_raw_impl().inner;

    return result;
}

} // namespace minilua::details

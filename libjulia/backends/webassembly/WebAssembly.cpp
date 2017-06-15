/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Alex Beregszaszi
 * @date 2017
 * Julia to WebAssembly code generator.
 */

#include <libjulia/backends/webassembly/WebAssembly.h>
#include <libjulia/backends/webassembly/IndentedWriter.h>
#include <libsolidity/inlineasm/AsmData.h>
#include <libsolidity/interface/Utils.h>

#include <libdevcore/CommonIO.h>

#include <binaryen-c.h>
#include <wasm-builder.h>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/count_if.hpp>

#include <memory>
#include <functional>

using namespace std;
using namespace dev;
using namespace dev::solidity;
using namespace dev::solidity::assembly;

class Generator: public boost::static_visitor<>
{
public:
	/// Create the code transformer which appends assembly to _state.assembly when called
	/// with parsed assembly data.
	/// @param _identifierAccess used to resolve identifiers external to the inline assembly
	explicit Generator(assembly::Block const& _block)
	{
		output.addLine("(module ");
		output.indent();
		visitStatements(_block);
		output.unindent();
		output.addLine(")");
	}

	string assembly() { return output.format(); }

public:
	void operator()(assembly::Instruction const&)
	{
		solAssert(false, "Instructions are not supported in Julia.");
	}
	void operator()(assembly::FunctionalInstruction const&)
	{
		solAssert(false, "Instructions are not supported in Julia.");
	}
	void operator()(assembly::StackAssignment const&)
	{
		solAssert(false, "Assignment from stack is not supported in Julia.");
	}
	void operator()(assembly::Label const&)
	{
		solAssert(false, "Labels are not supported in Julia.");
	}
	void operator()(assembly::Literal const& _literal)
	{
		if (_literal.kind == assembly::LiteralKind::Number)
			output.add("(" + convertType(_literal.type) + ".const " + _literal.value + ")");
		else if (_literal.kind == assembly::LiteralKind::Boolean)
			output.add("(" + convertType(_literal.type) + ".const " + string((_literal.value == "true") ? "1" : "0") + ")");
		else
			solAssert(false, "Non-number literals not supported.");
	}
	void operator()(assembly::Identifier const& _identifier)
	{
		output.add("(get_local $" + _identifier.name + ")");
	}
	void operator()(assembly::VariableDeclaration const& _varDecl)
	{
		solAssert(_varDecl.variables.size() == 1, "Tuples not supported yet.");
		output.addLine("(local $" + _varDecl.variables.front().name + " " + convertType(_varDecl.variables.front().type) + ")");
		output.addLine("(set_local $" + _varDecl.variables.front().name + " ");
		output.indent();
		boost::apply_visitor(*this, *_varDecl.value);
		output.unindent();
		output.add(")");
		output.newLine();
	}
	void operator()(assembly::Assignment const& _assignment)
	{
		output.addLine("(set_local $" + _assignment.variableName.name + " ");
		output.indent();
		boost::apply_visitor(*this, *_assignment.value);
		output.unindent();
		output.add(")");
		output.newLine();
	}
	void operator()(assembly::FunctionDefinition const& _funDef)
	{
		output.newLine();
		output.addLine("(func $" + _funDef.name + " ");
		output.indent();
		for (auto const& argument: _funDef.arguments)
			output.addLine("(param $" + argument.name + " " + convertType(argument.type) + ")");
		solAssert(_funDef.returns.size() <= 1, "Multiple return values not supported yet.");
		string returnName;
		for (auto const& returnArgument: _funDef.returns)
		{
			returnName = returnArgument.name;
			output.addLine("(result " + convertType(returnArgument.type) + ")");
			output.addLine("(local $" + returnArgument.name + " " + convertType(returnArgument.type) + ")");
		}
		/// Scope rules: return parameters must be marked appropriately
		output.newLine();
		output.newLine();
		visitStatements(_funDef.body);
		output.newLine();
		output.newLine();
		if (!returnName.empty())
			output.addLine("(return $" + returnName + ")");
		output.unindent();
		output.addLine(")");
		output.newLine();
	}
	void operator()(assembly::FunctionCall const& _funCall)
	{
		if (resolveBuiltinFunction(_funCall))
			return;

		output.addLine("(call $" + _funCall.functionName.name);
		output.indent();
		for (auto const& statement: _funCall.arguments)
		{
			output.add(" ");
			boost::apply_visitor(*this, statement);
			output.newLine();
		}
		output.unindent();
		output.addLine(")");
	}
	void operator()(assembly::Switch const& _switch)
	{
		solAssert(_switch.cases.size() <= 2, "");
		/// One of the cases must be the default case
		solAssert(
			_switch.cases[0].value ||
			_switch.cases[1].value,
			""
		);
		unsigned defaultcase = _switch.cases[0].value ? 0 : 1;
		solAssert(defaultcase <= (_switch.cases.size() - 1), "");

		output.addLine("(if (result i64) ");
		output.indent();
		output.add("(i64.eq ");
		boost::apply_visitor(*this, *_switch.expression);
		output.add(" ");
		(*this)(*(_switch.cases[!!defaultcase].value));
		output.add(")");
		output.newLine();
		output.add("(then ");
		output.indent();
		(*this)(_switch.cases[!!defaultcase].body);
		output.unindent();
		output.addLine(")");
		if (_switch.cases.size() == 2)
		{
			output.add("(else ");
			output.indent();
			(*this)(_switch.cases[defaultcase].body);
			output.unindent();
			output.addLine(")");
		}
		output.unindent();
		output.addLine(")");
	}
	void operator()(assembly::Block const& _block)
	{
		output.add("(block ");
		output.indent();
		visitStatements(_block);
		output.unindent();
		output.add(")");
	}
private:
	void visitStatements(assembly::Block const& _block)
	{
		std::for_each(_block.statements.begin(), _block.statements.end(), boost::apply_visitor(*this));
	}

	string convertType(assembly::Type type)
	{
		solAssert(!type.empty(), "Only Julia input is supported.");
		set<string> const supportedTypes{"bool", "u8", "s8", "u32", "s32", "u64", "s64"};
		solAssert(supportedTypes.count(type), "Type (" + type + ") not supported yet.");
		return "i64";
	}

	/// TODO: replace with a proper structure (and not manual code)
	bool resolveBuiltinFunction(assembly::FunctionCall const& _funCall)
	{
		if (_funCall.functionName.name == "add64")
		{
			output.add("(i64.add ");
			output.indent();
			solAssert(_funCall.arguments.size() == 2, "");
			boost::apply_visitor(*this, _funCall.arguments[0]);
			output.newLine();
			boost::apply_visitor(*this, _funCall.arguments[1]);
			output.unindent();
			output.add(")");
			return true;
		}
		else if (_funCall.functionName.name == "sub64")
		{
			output.add("(i64.sub ");
			output.indent();
			solAssert(_funCall.arguments.size() == 2, "");
			boost::apply_visitor(*this, _funCall.arguments[0]);
			output.newLine();
			boost::apply_visitor(*this, _funCall.arguments[1]);
			output.unindent();
			output.add(")");
			return true;
		}
		else if (_funCall.functionName.name == "mul64")
		{
			output.add("(i64.mul ");
			output.indent();
			solAssert(_funCall.arguments.size() == 2, "");
			boost::apply_visitor(*this, _funCall.arguments[0]);
			output.newLine();
			boost::apply_visitor(*this, _funCall.arguments[1]);
			output.unindent();
			output.add(")");
			return true;
		}
		else if (_funCall.functionName.name == "gt64")
		{
			output.add("(i64.gt_u ");
			output.indent();
			solAssert(_funCall.arguments.size() == 2, "");
			boost::apply_visitor(*this, _funCall.arguments[0]);
			output.newLine();
			boost::apply_visitor(*this, _funCall.arguments[1]);
			output.unindent();
			output.add(")");
			return true;
		}

		return false;
	}

	IndentedWriter output;
};

string julia::WebAssembly::assemble(assembly::Block const& _block)
{
#if 0
	BinaryenModuleRef module = BinaryenModuleCreate();

	// Create a function type for  i32 (i32, i32)
	BinaryenType params[2] = { BinaryenInt32(), BinaryenInt32() };
	BinaryenFunctionTypeRef iii = BinaryenAddFunctionType(module, "iii", BinaryenInt32(), params, 2);

	// Get the 0 and 1 arguments, and add them
	BinaryenExpressionRef x = BinaryenGetLocal(module, 0, BinaryenInt32()),
                        y = BinaryenGetLocal(module, 1, BinaryenInt32());
	BinaryenExpressionRef add = BinaryenBinary(module, BinaryenAddInt32(), x, y);

	// Create the add function
	// Note: no additional local variables
	// Note: no basic blocks here, we are an AST. The function body is just an expression node.
	BinaryenFunctionRef adder = BinaryenAddFunction(module, "adder", iii, NULL, 0, add);

	BinaryenSetFunctionTable(module, &adder, 1);

	// Print it out
	BinaryenModulePrint(module);

	// Clean up the module, which owns all the objects we created above
	BinaryenModuleDispose(module);
#endif

	return Generator(_block).assembly();
}

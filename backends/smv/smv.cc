/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct SmvWorker
{
	CellTypes ct;
	SigMap sigmap;
	RTLIL::Module *module;
	std::ostream &f;
	bool verbose;

	int idcounter;
	dict<IdString, shared_str> idcache;
	pool<shared_str> used_names;
	vector<shared_str> strbuf;

	pool<Wire*> partial_assignment_wires;
	dict<SigBit, std::pair<const char*, int>> partial_assignment_bits;
	vector<string> inputvars, vars, definitions, assignments, invarspecs;

	const char *cid()
	{
		while (true) {
			shared_str s(stringf("_%d", idcounter++));
			if (!used_names.count(s)) {
				used_names.insert(s);
				return s.c_str();
			}
		}
	}

	const char *cid(IdString id, bool precache = false)
	{
		if (!idcache.count(id))
		{
			string name = stringf("_%s", id.c_str());

			if (name.compare(0, 2, "_\\") == 0)
				name = "_" + name.substr(2);

			for (auto &c : name) {
				if (c == '|' || c == '$' || c == '_') continue;
				if (c >= 'a' && c <='z') continue;
				if (c >= 'A' && c <='Z') continue;
				if (c >= '0' && c <='9') continue;
				if (precache) return nullptr;
				c = '#';
			}

			if (name == "_main")
				name = "main";

			while (used_names.count(name))
				name += "_";

			shared_str s(name);
			used_names.insert(s);
			idcache[id] = s;
		}

		return idcache.at(id).c_str();
	}

	SmvWorker(RTLIL::Module *module, bool verbose, std::ostream &f) :
			ct(module->design), sigmap(module), module(module), f(f), verbose(verbose), idcounter(0)
	{
		for (auto mod : module->design->modules())
			cid(mod->name, true);

		for (auto wire : module->wires())
			cid(wire->name, true);

		for (auto cell : module->cells()) {
			cid(cell->name, true);
			cid(cell->type, true);
			for (auto &conn : cell->connections())
				cid(conn.first, true);
		}
	}

	const char *rvalue(SigSpec sig, int width = -1, bool is_signed = false)
	{
		string s;
		int count_chunks = 0;
		sigmap.apply(sig);

		for (int i = 0; i < GetSize(sig); i++)
			if (partial_assignment_bits.count(sig[i]))
			{
				int width = 1;
				const auto &bit_a = partial_assignment_bits.at(sig[i]);

				while (i+width < GetSize(sig))
				{
					if (!partial_assignment_bits.count(sig[i+width]))
						break;

					const auto &bit_b = partial_assignment_bits.at(sig[i+width]);
					if (strcmp(bit_a.first, bit_b.first))
						break;
					if (bit_a.second+width != bit_b.second)
						break;

					width++;
				}

				if (i+width < GetSize(sig))
					s = stringf("%s :: ", rvalue(sig.extract(i+width, GetSize(sig)-(width+i))));

				s += stringf("%s[%d:%d]", bit_a.first, bit_a.second+width-1, bit_a.second);

				if (i > 0)
					s += stringf(" :: %s", rvalue(sig.extract(0, i)));

				count_chunks = 3;
				goto continue_with_resize;
			}

		for (auto &c : sig.chunks()) {
			count_chunks++;
			if (!s.empty())
				s = " :: " + s;
			if (c.wire) {
				if (c.offset != 0 || c.width != c.wire->width)
					s = stringf("%s[%d:%d]", cid(c.wire->name), c.offset+c.width-1, c.offset) + s;
				else
					s = cid(c.wire->name) + s;
			} else {
				string v = stringf("0ub%d_", c.width);
				for (int i = c.width-1; i >= 0; i--)
					v += c.data.at(i) == State::S1 ? '1' : '0';
				s = v + s;
			}
		}

	continue_with_resize:;
		if (width >= 0) {
			if (is_signed) {
				if (GetSize(sig) > width)
					s = stringf("signed(resize(%s, %d))", s.c_str(), width);
				else
					s = stringf("resize(signed(%s), %d)", s.c_str(), width);
			} else
				s = stringf("resize(%s, %d)", s.c_str(), width);
		} else if (is_signed)
			s = stringf("signed(%s)", s.c_str());
		else if (count_chunks > 1)
			s = stringf("(%s)", s.c_str());

		strbuf.push_back(s);
		return strbuf.back().c_str();
	}

	const char *rvalue_u(SigSpec sig, int width = -1)
	{
		return rvalue(sig, width, false);
	}

	const char *rvalue_s(SigSpec sig, int width = -1, bool is_signed = true)
	{
		return rvalue(sig, width, is_signed);
	}

	const char *lvalue(SigSpec sig)
	{
		sigmap.apply(sig);

		if (sig.is_wire())
			return rvalue(sig);

		const char *temp_id = cid();
//		f << stringf("    %s : unsigned word[%d]; -- %s\n", temp_id, GetSize(sig), log_signal(sig));

		int offset = 0;
		for (auto bit : sig) {
			log_assert(bit.wire != nullptr);
			partial_assignment_wires.insert(bit.wire);
			partial_assignment_bits[bit] = std::pair<const char*, int>(temp_id, offset++);
		}

		return temp_id;
	}

	void run()
	{
		f << stringf("MODULE %s\n", cid(module->name));

		for (auto wire : module->wires())
		{
			if (SigSpec(wire) != sigmap(wire))
				partial_assignment_wires.insert(wire);

			if (wire->port_input)
				inputvars.push_back(stringf("%s : unsigned word[%d]; -- %s", cid(wire->name), wire->width, log_id(wire)));

			if (wire->attributes.count(ID::init))
				assignments.push_back(stringf("init(%s) := %s;", lvalue(wire), rvalue(wire->attributes.at(ID::init))));
		}

		for (auto cell : module->cells())
		{
			// FIXME: $slice, $concat, $mem

			if (cell->type.in(ID($assert)))
			{
				SigSpec sig_a = cell->getPort(ID::A);
				SigSpec sig_en = cell->getPort(ID::EN);

				invarspecs.push_back(stringf("!bool(%s) | bool(%s);", rvalue(sig_en), rvalue(sig_a)));

				continue;
			}

			if (cell->type.in(ID($shl), ID($shr), ID($sshl), ID($sshr), ID($shift), ID($shiftx)))
			{
				SigSpec sig_a = cell->getPort(ID::A);
				SigSpec sig_b = cell->getPort(ID::B);

				int width_y = GetSize(cell->getPort(ID::Y));
				int shift_b_width = GetSize(sig_b);
				int width_ay = max(GetSize(sig_a), width_y);
				int width = width_ay;

				for (int i = 1, j = 0;; i <<= 1, j++)
					if (width_ay < i) {
						width = i-1;
						shift_b_width = min(shift_b_width, j);
						break;
					}

				bool signed_a = cell->getParam(ID::A_SIGNED).as_bool();
				bool signed_b = cell->getParam(ID::B_SIGNED).as_bool();
				string op = cell->type.in(ID($shl), ID($sshl)) ? "<<" : ">>";
				string expr, expr_a;

				if (cell->type == ID($sshr) && signed_a)
				{
					expr_a = rvalue_s(sig_a, width);
					expr = stringf("resize(unsigned(%s %s %s), %d)", expr_a.c_str(), op.c_str(), rvalue(sig_b.extract(0, shift_b_width)), width_y);
					if (shift_b_width < GetSize(sig_b))
						expr = stringf("%s != 0ud%d_0 ? (bool(%s) ? !0ud%d_0 : 0ud%d_0) : %s",
								rvalue(sig_b.extract(shift_b_width, GetSize(sig_b) - shift_b_width)), GetSize(sig_b) - shift_b_width,
								rvalue(sig_a[GetSize(sig_a)-1]), width_y, width_y, expr.c_str());
				}
				else if (cell->type.in(ID($shift), ID($shiftx)) && signed_b)
				{
					expr_a = rvalue_u(sig_a, width);

					const char *b_shr = rvalue_u(sig_b);
					const char *b_shl = cid();

//					f << stringf("    %s : unsigned word[%d]; -- neg(%s)\n", b_shl, GetSize(sig_b), log_signal(sig_b));
					definitions.push_back(stringf("%s := unsigned(-%s);", b_shl, rvalue_s(sig_b)));

					string expr_shl = stringf("resize(%s << %s[%d:0], %d)", expr_a.c_str(), b_shl, shift_b_width-1, width_y);
					string expr_shr = stringf("resize(%s >> %s[%d:0], %d)", expr_a.c_str(), b_shr, shift_b_width-1, width_y);

					if (shift_b_width < GetSize(sig_b)) {
						expr_shl = stringf("%s[%d:%d] != 0ud%d_0 ? 0ud%d_0 : %s", b_shl, GetSize(sig_b)-1, shift_b_width,
								GetSize(sig_b)-shift_b_width, width_y, expr_shl.c_str());
						expr_shr = stringf("%s[%d:%d] != 0ud%d_0 ? 0ud%d_0 : %s", b_shr, GetSize(sig_b)-1, shift_b_width,
								GetSize(sig_b)-shift_b_width, width_y, expr_shr.c_str());
					}

					expr = stringf("bool(%s) ? %s : %s", rvalue(sig_b[GetSize(sig_b)-1]), expr_shl.c_str(), expr_shr.c_str());
				}
				else
				{
					if (cell->type.in(ID($shift), ID($shiftx)) || !signed_a)
						expr_a = rvalue_u(sig_a, width);
					else
						expr_a = stringf("resize(unsigned(%s), %d)", rvalue_s(sig_a, width_ay), width);

					expr = stringf("resize(%s %s %s[%d:0], %d)", expr_a.c_str(), op.c_str(), rvalue_u(sig_b), shift_b_width-1, width_y);
					if (shift_b_width < GetSize(sig_b))
						expr = stringf("%s[%d:%d] != 0ud%d_0 ? 0ud%d_0 : %s", rvalue_u(sig_b), GetSize(sig_b)-1, shift_b_width,
								GetSize(sig_b)-shift_b_width, width_y, expr.c_str());
				}

				definitions.push_back(stringf("%s := %s;", lvalue(cell->getPort(ID::Y)), expr.c_str()));

				continue;
			}

			if (cell->type.in(ID($not), ID($pos), ID($neg)))
			{
				int width = GetSize(cell->getPort(ID::Y));
				string expr_a, op;

				if (cell->type == ID($not))  op = "!";
				if (cell->type == ID($pos))  op = "";
				if (cell->type == ID($neg))  op = "-";

				if (cell->getParam(ID::A_SIGNED).as_bool())
				{
					definitions.push_back(stringf("%s := unsigned(%s%s);", lvalue(cell->getPort(ID::Y)),
							op.c_str(), rvalue_s(cell->getPort(ID::A), width)));
				}
				else
				{
					definitions.push_back(stringf("%s := %s%s;", lvalue(cell->getPort(ID::Y)),
							op.c_str(), rvalue_u(cell->getPort(ID::A), width)));
				}

				continue;
			}

			if (cell->type.in(ID($add), ID($sub), ID($mul), ID($and), ID($or), ID($xor), ID($xnor)))
			{
				int width = GetSize(cell->getPort(ID::Y));
				string expr_a, expr_b, op;

				if (cell->type == ID($add))  op = "+";
				if (cell->type == ID($sub))  op = "-";
				if (cell->type == ID($mul))  op = "*";
				if (cell->type == ID($and))  op = "&";
				if (cell->type == ID($or))   op = "|";
				if (cell->type == ID($xor))  op = "xor";
				if (cell->type == ID($xnor)) op = "xnor";

				if (cell->getParam(ID::A_SIGNED).as_bool())
				{
					definitions.push_back(stringf("%s := unsigned(%s %s %s);", lvalue(cell->getPort(ID::Y)),
							rvalue_s(cell->getPort(ID::A), width), op.c_str(), rvalue_s(cell->getPort(ID::B), width)));
				}
				else
				{
					definitions.push_back(stringf("%s := %s %s %s;", lvalue(cell->getPort(ID::Y)),
							rvalue_u(cell->getPort(ID::A), width), op.c_str(), rvalue_u(cell->getPort(ID::B), width)));
				}

				continue;
			}

			// SMV has a "mod" operator, but its semantics don't seem to be well-defined - to be safe, don't generate it at all
			if (cell->type.in(ID($div)/*, ID($mod), ID($modfloor)*/))
			{
				int width_y = GetSize(cell->getPort(ID::Y));
				int width = max(width_y, GetSize(cell->getPort(ID::A)));
				width = max(width, GetSize(cell->getPort(ID::B)));
				string expr_a, expr_b, op;

				if (cell->type == ID($div))  op = "/";
				//if (cell->type == ID($mod))  op = "mod";

				if (cell->getParam(ID::A_SIGNED).as_bool())
				{
					definitions.push_back(stringf("%s := resize(unsigned(%s %s %s), %d);", lvalue(cell->getPort(ID::Y)),
							rvalue_s(cell->getPort(ID::A), width), op.c_str(), rvalue_s(cell->getPort(ID::B), width), width_y));
				}
				else
				{
					definitions.push_back(stringf("%s := resize(%s %s %s, %d);", lvalue(cell->getPort(ID::Y)),
							rvalue_u(cell->getPort(ID::A), width), op.c_str(), rvalue_u(cell->getPort(ID::B), width), width_y));
				}

				continue;
			}

			if (cell->type.in(ID($eq), ID($ne), ID($eqx), ID($nex), ID($lt), ID($le), ID($ge), ID($gt)))
			{
				int width = max(GetSize(cell->getPort(ID::A)), GetSize(cell->getPort(ID::B)));
				string expr_a, expr_b, op;

				if (cell->type == ID($eq))  op = "=";
				if (cell->type == ID($ne))  op = "!=";
				if (cell->type == ID($eqx)) op = "=";
				if (cell->type == ID($nex)) op = "!=";
				if (cell->type == ID($lt))  op = "<";
				if (cell->type == ID($le))  op = "<=";
				if (cell->type == ID($ge))  op = ">=";
				if (cell->type == ID($gt))  op = ">";

				if (cell->getParam(ID::A_SIGNED).as_bool())
				{
					expr_a = stringf("resize(signed(%s), %d)", rvalue(cell->getPort(ID::A)), width);
					expr_b = stringf("resize(signed(%s), %d)", rvalue(cell->getPort(ID::B)), width);
				}
				else
				{
					expr_a = stringf("resize(%s, %d)", rvalue(cell->getPort(ID::A)), width);
					expr_b = stringf("resize(%s, %d)", rvalue(cell->getPort(ID::B)), width);
				}

				definitions.push_back(stringf("%s := resize(word1(%s %s %s), %d);", lvalue(cell->getPort(ID::Y)),
						expr_a.c_str(), op.c_str(), expr_b.c_str(), GetSize(cell->getPort(ID::Y))));

				continue;
			}

			if (cell->type.in(ID($reduce_and), ID($reduce_or), ID($reduce_bool)))
			{
				int width_a = GetSize(cell->getPort(ID::A));
				int width_y = GetSize(cell->getPort(ID::Y));
				const char *expr_a = rvalue(cell->getPort(ID::A));
				const char *expr_y = lvalue(cell->getPort(ID::Y));
				string expr;

				if (cell->type == ID($reduce_and))  expr = stringf("%s = !0ub%d_0", expr_a, width_a);
				if (cell->type == ID($reduce_or))   expr = stringf("%s != 0ub%d_0", expr_a, width_a);
				if (cell->type == ID($reduce_bool)) expr = stringf("%s != 0ub%d_0", expr_a, width_a);

				definitions.push_back(stringf("%s := resize(word1(%s), %d);", expr_y, expr.c_str(), width_y));
				continue;
			}

			if (cell->type.in(ID($reduce_xor), ID($reduce_xnor)))
			{
				int width_y = GetSize(cell->getPort(ID::Y));
				const char *expr_y = lvalue(cell->getPort(ID::Y));
				string expr;

				for (auto bit : cell->getPort(ID::A)) {
					if (!expr.empty())
						expr += " xor ";
					expr += rvalue(bit);
				}

				if (cell->type == ID($reduce_xnor))
					expr = "!(" + expr + ")";

				definitions.push_back(stringf("%s := resize(%s, %d);", expr_y, expr.c_str(), width_y));
				continue;
			}

			if (cell->type.in(ID($logic_and), ID($logic_or)))
			{
				int width_a = GetSize(cell->getPort(ID::A));
				int width_b = GetSize(cell->getPort(ID::B));
				int width_y = GetSize(cell->getPort(ID::Y));

				string expr_a = stringf("(%s != 0ub%d_0)", rvalue(cell->getPort(ID::A)), width_a);
				string expr_b = stringf("(%s != 0ub%d_0)", rvalue(cell->getPort(ID::B)), width_b);
				const char *expr_y = lvalue(cell->getPort(ID::Y));

				string expr;
				if (cell->type == ID($logic_and)) expr = expr_a + " & " + expr_b;
				if (cell->type == ID($logic_or))  expr = expr_a + " | " + expr_b;

				definitions.push_back(stringf("%s := resize(word1(%s), %d);", expr_y, expr.c_str(), width_y));
				continue;
			}

			if (cell->type.in(ID($logic_not)))
			{
				int width_a = GetSize(cell->getPort(ID::A));
				int width_y = GetSize(cell->getPort(ID::Y));

				string expr_a = stringf("(%s = 0ub%d_0)", rvalue(cell->getPort(ID::A)), width_a);
				const char *expr_y = lvalue(cell->getPort(ID::Y));

				definitions.push_back(stringf("%s := resize(word1(%s), %d);", expr_y, expr_a.c_str(), width_y));
				continue;
			}

			if (cell->type.in(ID($mux), ID($pmux)))
			{
				int width = GetSize(cell->getPort(ID::Y));
				SigSpec sig_a = cell->getPort(ID::A);
				SigSpec sig_b = cell->getPort(ID::B);
				SigSpec sig_s = cell->getPort(ID::S);

				string expr;
				for (int i = 0; i < GetSize(sig_s); i++)
					expr += stringf("bool(%s) ? %s : ", rvalue(sig_s[i]), rvalue(sig_b.extract(i*width, width)));
				expr += rvalue(sig_a);

				definitions.push_back(stringf("%s := %s;", lvalue(cell->getPort(ID::Y)), expr.c_str()));
				continue;
			}

			if (cell->type == ID($dff))
			{
				vars.push_back(stringf("%s : unsigned word[%d]; -- %s", lvalue(cell->getPort(ID::Q)), GetSize(cell->getPort(ID::Q)), log_signal(cell->getPort(ID::Q))));
				assignments.push_back(stringf("next(%s) := %s;", lvalue(cell->getPort(ID::Q)), rvalue(cell->getPort(ID::D))));
				continue;
			}

			if (cell->type.in(ID($_BUF_), ID($_NOT_)))
			{
				string op = cell->type == ID($_NOT_) ? "!" : "";
				definitions.push_back(stringf("%s := %s%s;", lvalue(cell->getPort(ID::Y)), op.c_str(), rvalue(cell->getPort(ID::A))));
				continue;
			}

			if (cell->type.in(ID($_AND_), ID($_NAND_), ID($_OR_), ID($_NOR_), ID($_XOR_), ID($_XNOR_), ID($_ANDNOT_), ID($_ORNOT_)))
			{
				string op;

				if (cell->type.in(ID($_AND_), ID($_NAND_), ID($_ANDNOT_))) op = "&";
				if (cell->type.in(ID($_OR_), ID($_NOR_), ID($_ORNOT_))) op = "|";
				if (cell->type.in(ID($_XOR_)))  op = "xor";
				if (cell->type.in(ID($_XNOR_)))  op = "xnor";

				if (cell->type.in(ID($_ANDNOT_), ID($_ORNOT_)))
					definitions.push_back(stringf("%s := %s %s (!%s);", lvalue(cell->getPort(ID::Y)),
							rvalue(cell->getPort(ID::A)), op.c_str(), rvalue(cell->getPort(ID::B))));
				else
				if (cell->type.in(ID($_NAND_), ID($_NOR_)))
					definitions.push_back(stringf("%s := !(%s %s %s);", lvalue(cell->getPort(ID::Y)),
							rvalue(cell->getPort(ID::A)), op.c_str(), rvalue(cell->getPort(ID::B))));
				else
					definitions.push_back(stringf("%s := %s %s %s;", lvalue(cell->getPort(ID::Y)),
							rvalue(cell->getPort(ID::A)), op.c_str(), rvalue(cell->getPort(ID::B))));
				continue;
			}

			if (cell->type == ID($_MUX_))
			{
				definitions.push_back(stringf("%s := bool(%s) ? %s : %s;", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::S)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::A))));
				continue;
			}

			if (cell->type == ID($_NMUX_))
			{
				definitions.push_back(stringf("%s := !(bool(%s) ? %s : %s);", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::S)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::A))));
				continue;
			}

			if (cell->type == ID($_AOI3_))
			{
				definitions.push_back(stringf("%s := !((%s & %s) | %s);", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::A)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::C))));
				continue;
			}

			if (cell->type == ID($_OAI3_))
			{
				definitions.push_back(stringf("%s := !((%s | %s) & %s);", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::A)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::C))));
				continue;
			}

			if (cell->type == ID($_AOI4_))
			{
				definitions.push_back(stringf("%s := !((%s & %s) | (%s & %s));", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::A)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::C)), rvalue(cell->getPort(ID::D))));
				continue;
			}

			if (cell->type == ID($_OAI4_))
			{
				definitions.push_back(stringf("%s := !((%s | %s) & (%s | %s));", lvalue(cell->getPort(ID::Y)),
						rvalue(cell->getPort(ID::A)), rvalue(cell->getPort(ID::B)), rvalue(cell->getPort(ID::C)), rvalue(cell->getPort(ID::D))));
				continue;
			}

			if (cell->type == ID($scopeinfo))
				continue;

			if (cell->type[0] == '$') {
				if (cell->type.in(ID($dffe), ID($sdff), ID($sdffe), ID($sdffce)) || cell->type.str().substr(0, 6) == "$_SDFF" || (cell->type.str().substr(0, 6) == "$_DFFE" && cell->type.str().size() == 10)) {
					log_error("Unsupported cell type %s for cell %s.%s -- please run `dffunmap` before `write_smv`.\n",
							log_id(cell->type), log_id(module), log_id(cell));
				}
				if (cell->type.in(ID($adff), ID($adffe), ID($aldff), ID($aldffe), ID($dffsr), ID($dffsre)) || cell->type.str().substr(0, 5) == "$_DFF" || cell->type.str().substr(0, 7) == "$_ALDFF") {
					log_error("Unsupported cell type %s for cell %s.%s -- please run `async2sync; dffunmap` or `clk2fflogic` before `write_smv`.\n",
							log_id(cell->type), log_id(module), log_id(cell));
				}
				if (cell->type.in(ID($sr), ID($dlatch), ID($adlatch), ID($dlatchsr)) || cell->type.str().substr(0, 8) == "$_DLATCH" || cell->type.str().substr(0, 5) == "$_SR_") {
					log_error("Unsupported cell type %s for cell %s.%s -- please run `clk2fflogic` before `write_smv`.\n",
							log_id(cell->type), log_id(module), log_id(cell));
				}
				log_error("Unsupported cell type %s for cell %s.%s.\n",
						log_id(cell->type), log_id(module), log_id(cell));
			}

//			f << stringf("    %s : %s;\n", cid(cell->name), cid(cell->type));

			for (auto &conn : cell->connections())
				if (cell->output(conn.first))
					definitions.push_back(stringf("%s := %s.%s;", lvalue(conn.second), cid(cell->name), cid(conn.first)));
				else
					definitions.push_back(stringf("%s.%s := %s;", cid(cell->name), cid(conn.first), rvalue(conn.second)));
		}

		for (Wire *wire : partial_assignment_wires)
		{
			string expr;

			for (int i = 0; i < wire->width; i++)
			{
				if (!expr.empty())
					expr = " :: " + expr;

				if (partial_assignment_bits.count(sigmap(SigBit(wire, i))))
				{
					int width = 1;
					const auto &bit_a = partial_assignment_bits.at(sigmap(SigBit(wire, i)));

					while (i+1 < wire->width)
					{
						SigBit next_bit = sigmap(SigBit(wire, i+1));

						if (!partial_assignment_bits.count(next_bit))
							break;

						const auto &bit_b = partial_assignment_bits.at(next_bit);
						if (strcmp(bit_a.first, bit_b.first))
							break;
						if (bit_a.second+width != bit_b.second)
							break;

						width++, i++;
					}

					expr = stringf("%s[%d:%d]", bit_a.first, bit_a.second+width-1, bit_a.second) + expr;
				}
				else if (sigmap(SigBit(wire, i)).wire == nullptr)
				{
					string bits;
					SigSpec sig = sigmap(SigSpec(wire, i));

					while (i+1 < wire->width) {
						SigBit next_bit = sigmap(SigBit(wire, i+1));
						if (next_bit.wire != nullptr)
							break;
						sig.append(next_bit);
						i++;
					}

					for (int k = GetSize(sig)-1; k >= 0; k--)
						bits += sig[k] == State::S1 ? '1' : '0';

					expr = stringf("0ub%d_%s", GetSize(bits), bits.c_str()) + expr;
				}
				else if (sigmap(SigBit(wire, i)) == SigBit(wire, i))
				{
					int length = 1;

					while (i+1 < wire->width) {
						if (partial_assignment_bits.count(sigmap(SigBit(wire, i+1))))
							break;
						if (sigmap(SigBit(wire, i+1)) != SigBit(wire, i+1))
							break;
						i++, length++;
					}

					expr = stringf("0ub%d_0", length) + expr;
				}
				else
				{
					string bits;
					SigSpec sig = sigmap(SigSpec(wire, i));

					while (i+1 < wire->width) {
						SigBit next_bit = sigmap(SigBit(wire, i+1));
						if (next_bit.wire == nullptr || partial_assignment_bits.count(next_bit))
							break;
						sig.append(next_bit);
						i++;
					}

					expr = rvalue(sig) + expr;
				}
			}

			definitions.push_back(stringf("%s := %s;", cid(wire->name), expr.c_str()));
		}

		if (!inputvars.empty()) {
			f << stringf("  IVAR\n");
			for (const string &line : inputvars)
				f << stringf("    %s\n", line.c_str());
		}

		if (!vars.empty()) {
			f << stringf("  VAR\n");
			for (const string &line : vars)
				f << stringf("    %s\n", line.c_str());
		}

		if (!definitions.empty()) {
			f << stringf("  DEFINE\n");
			for (const string &line : definitions)
				f << stringf("    %s\n", line.c_str());
		}

		if (!assignments.empty()) {
			f << stringf("  ASSIGN\n");
			for (const string &line : assignments)
				f << stringf("    %s\n", line.c_str());
		}

		if (!invarspecs.empty()) {
			for (const string &line : invarspecs)
				f << stringf("  INVARSPEC %s\n", line.c_str());
		}
	}
};

struct SmvBackend : public Backend {
	SmvBackend() : Backend("smv", "write design to SMV file") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_smv [options] [filename]\n");
		log("\n");
		log("Write an SMV description of the current design.\n");
		log("\n");
		log("    -verbose\n");
		log("        this will print the recursive walk used to export the modules.\n");
		log("\n");
		log("    -tpl <template_file>\n");
		log("        use the given template file. the line containing only the token '%%%%'\n");
		log("        is replaced with the regular output of this command.\n");
		log("\n");
		log("THIS COMMAND IS UNDER CONSTRUCTION\n");
		log("\n");
	}
	void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
	{
		std::ifstream template_f;
		bool verbose = false;

		log_header(design, "Executing SMV backend.\n");

		log_push();
		Pass::call(design, "bmuxmap");
		Pass::call(design, "demuxmap");
		Pass::call(design, "bwmuxmap");
		log_pop();

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-tpl" && argidx+1 < args.size()) {
				template_f.open(args[++argidx]);
				if (template_f.fail())
					log_error("Can't open template file `%s'.\n", args[argidx].c_str());
				continue;
			}
			if (args[argidx] == "-verbose") {
				verbose = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		pool<Module*> modules;

		for (auto module : design->modules())
			if (!module->get_blackbox_attribute() && !module->has_memories_warn() && !module->has_processes_warn())
				modules.insert(module);

		if (template_f.is_open())
		{
			std::string line;
			while (std::getline(template_f, line))
			{
				int indent = 0;
				while (indent < GetSize(line) && (line[indent] == ' ' || line[indent] == '\t'))
					indent++;

				if (line[indent] == '%')
				{
					vector<string> stmt = split_tokens(line);

					if (GetSize(stmt) == 1 && stmt[0] == "%%")
						break;

					if (GetSize(stmt) == 2 && stmt[0] == "%module")
					{
						Module *module = design->module(RTLIL::escape_id(stmt[1]));
						modules.erase(module);

						if (module == nullptr)
							log_error("Module '%s' not found.\n", stmt[1].c_str());

						*f << stringf("-- SMV description generated by %s\n", yosys_maybe_version());

						log("Creating SMV representation of module %s.\n", log_id(module));
						SmvWorker worker(module, verbose, *f);
						worker.run();

						*f << stringf("-- end of yosys output\n");
						continue;
					}

					log_error("Unknown template statement: '%s'", line.c_str() + indent);
				}

				*f << line << std::endl;
			}
		}

		if (!modules.empty())
		{
			*f << stringf("-- SMV description generated by %s\n", yosys_maybe_version());

			for (auto module : modules) {
				log("Creating SMV representation of module %s.\n", log_id(module));
				SmvWorker worker(module, verbose, *f);
				worker.run();
			}

			*f << stringf("-- end of yosys output\n");
		}

		if (template_f.is_open()) {
			std::string line;
			while (std::getline(template_f, line))
				*f << line << std::endl;
		}
	}
} SmvBackend;

PRIVATE_NAMESPACE_END

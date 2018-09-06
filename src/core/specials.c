/*
* Copyright (c) 2018 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#include <janet/janet.h>
#include "compile.h"
#include "util.h"
#include "vector.h"
#include "emit.h"

static JanetSlot janetc_quote(JanetFopts opts, int32_t argn, const Janet *argv) {
    if (argn != 1) {
        janetc_cerror(opts.compiler, "expected 1 argument");
        return janetc_cslot(janet_wrap_nil());
    }
    return janetc_cslot(argv[0]);
}

/* Preform destructuring. Be careful to
 * keep the order registers are freed. 
 * Returns if the slot 'right' can be freed. */
static int destructure(JanetCompiler *c,
        Janet left,
        JanetSlot right,
        int (*leaf)(JanetCompiler *c,
            const uint8_t *sym,
            JanetSlot s,
            JanetTable *attr),
        JanetTable *attr) {
    switch (janet_type(left)) {
        default:
            janetc_cerror(c, "unexpected type in destructuring");
            return 1;
        case JANET_SYMBOL:
            /* Leaf, assign right to left */
            return leaf(c, janet_unwrap_symbol(left), right, attr);
        case JANET_TUPLE:
        case JANET_ARRAY:
            {
                int32_t i, len;
                const Janet *values;
                janet_indexed_view(left, &values, &len);
                for (i = 0; i < len; i++) {
                    JanetSlot nextright = janetc_farslot(c);
                    Janet subval = values[i];
                    if (i < 0x100) {
                        janetc_emit_ssu(c, JOP_GET_INDEX, nextright, right, (uint8_t) i, 1);
                    } else {
                        JanetSlot k = janetc_cslot(janet_wrap_integer(i));
                        janetc_emit_sss(c, JOP_GET, nextright, right, k, 1);
                    }
                    if (destructure(c, subval, nextright, leaf, attr))
                        janetc_freeslot(c, nextright);
                }
            }
            return 1;
        case JANET_TABLE:
        case JANET_STRUCT:
            {
                const JanetKV *kvs = NULL;
                int32_t i, cap, len;
                janet_dictionary_view(left, &kvs, &len, &cap);
                for (i = 0; i < cap; i++) {
                    if (janet_checktype(kvs[i].key, JANET_NIL)) continue;
                    JanetSlot nextright = janetc_farslot(c);
                    JanetSlot k = janetc_value(janetc_fopts_default(c), kvs[i].key);
                    janetc_emit_sss(c, JOP_GET, nextright, right, k, 1);
                    if (destructure(c, kvs[i].value, nextright, leaf, attr))
                        janetc_freeslot(c, nextright);
                }
            }
            return 1;
    }
}

static JanetSlot janetc_varset(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetFopts subopts = janetc_fopts_default(opts.compiler);
    JanetSlot ret, dest;
    Janet head;
    if (argn != 2) {
        janetc_cerror(opts.compiler, "expected 2 arguments");
        return janetc_cslot(janet_wrap_nil());
    }
    head = argv[0];
    if (!janet_checktype(head, JANET_SYMBOL)) {
        janetc_cerror(opts.compiler, "expected symbol");
        return janetc_cslot(janet_wrap_nil());
    }
    dest = janetc_resolve(opts.compiler, janet_unwrap_symbol(head));
    if (!(dest.flags & JANET_SLOT_MUTABLE)) {
        janetc_cerror(opts.compiler, "cannot set constant");
        return janetc_cslot(janet_wrap_nil());
    }
    subopts.flags = JANET_FOPTS_HINT;
    subopts.hint = dest;
    ret = janetc_value(subopts, argv[1]);
    janetc_copy(opts.compiler, dest, ret);
    return ret;
}

/* Add attributes to a global def or var table */
static JanetTable *handleattr(JanetCompiler *c, int32_t argn, const Janet *argv) {
    int32_t i;
    JanetTable *tab = janet_table(2);
    for (i = 1; i < argn - 1; i++) {
        Janet attr = argv[i];
        switch (janet_type(attr)) {
            default:
                janetc_cerror(c, "could not add metadata to binding");
                break;
            case JANET_SYMBOL:
                janet_table_put(tab, attr, janet_wrap_true());
                break;
            case JANET_STRING:
                janet_table_put(tab, janet_csymbolv("doc"), attr);
                break;
        }
    }
    return tab;
}

static JanetSlot dohead(JanetCompiler *c, JanetFopts opts, Janet *head, int32_t argn, const Janet *argv) {
    JanetFopts subopts = janetc_fopts_default(c);
    JanetSlot ret;
    if (argn < 2) {
        janetc_cerror(c, "expected at least 2 arguments");
        return janetc_cslot(janet_wrap_nil());
    }
    *head = argv[0];
    subopts.flags = opts.flags & ~(JANET_FOPTS_TAIL | JANET_FOPTS_DROP);
    subopts.hint = opts.hint;
    ret = janetc_value(subopts, argv[argn - 1]);
    return ret;
}

/* Def or var a symbol in a local scope */
static int namelocal(JanetCompiler *c, const uint8_t *head, int32_t flags, JanetSlot ret) {
    int isUnnamedRegister = !(ret.flags & JANET_SLOT_NAMED) &&
        ret.index > 0 &&
        ret.envindex >= 0;
    if (!isUnnamedRegister) {
        /* Slot is not able to be named */
        JanetSlot localslot = janetc_farslot(c);
        janetc_copy(c, localslot, ret);
        ret = localslot;
    }
    ret.flags |= flags;
    janetc_nameslot(c, head, ret);
    return !isUnnamedRegister;
}

static int varleaf(
        JanetCompiler *c,
        const uint8_t *sym,
        JanetSlot s,
        JanetTable *attr) {
    if (c->scope->flags & JANET_SCOPE_TOP) {
        /* Global var, generate var */
        JanetSlot refslot;
        JanetTable *reftab = janet_table(1);
        reftab->proto = attr;
        JanetArray *ref = janet_array(1);
        janet_array_push(ref, janet_wrap_nil());
        janet_table_put(reftab, janet_csymbolv(":ref"), janet_wrap_array(ref));
        janet_table_put(c->env, janet_wrap_symbol(sym), janet_wrap_table(reftab));
        refslot = janetc_cslot(janet_wrap_array(ref));
        janetc_emit_ssu(c, JOP_PUT_INDEX, refslot, s, 0, 0);
        return 1;
    } else {
        return namelocal(c, sym, JANET_SLOT_MUTABLE, s);
    }
}

static JanetSlot janetc_var(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetCompiler *c = opts.compiler;
    Janet head;
    JanetSlot ret = dohead(c, opts, &head, argn, argv);
    if (c->result.status == JANET_COMPILE_ERROR)
        return janetc_cslot(janet_wrap_nil());
    if (destructure(c, argv[0], ret, varleaf, handleattr(c, argn, argv)))
        janetc_freeslot(c, ret);
    return janetc_cslot(janet_wrap_nil());
}

static int defleaf(
        JanetCompiler *c,
        const uint8_t *sym,
        JanetSlot s,
        JanetTable *attr) {
    if (c->scope->flags & JANET_SCOPE_TOP) {
        JanetTable *tab = janet_table(2);
        tab->proto = attr;
        JanetSlot valsym = janetc_cslot(janet_csymbolv(":value"));
        JanetSlot tabslot = janetc_cslot(janet_wrap_table(tab));

        /* Add env entry to env */
        janet_table_put(c->env, janet_wrap_symbol(sym), janet_wrap_table(tab));

        /* Put value in table when evaulated */
        janetc_emit_sss(c, JOP_PUT, tabslot, valsym, s, 0);
        return 1;
    } else {
        return namelocal(c, sym, 0, s);
    }
}

static JanetSlot janetc_def(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetCompiler *c = opts.compiler;
    Janet head;
    opts.flags &= ~JANET_FOPTS_HINT;
    JanetSlot ret = dohead(c, opts, &head, argn, argv);
    if (c->result.status == JANET_COMPILE_ERROR)
        return janetc_cslot(janet_wrap_nil());
    if (destructure(c, argv[0], ret, defleaf, handleattr(c, argn, argv)))
        janetc_freeslot(c, ret);
    return janetc_cslot(janet_wrap_nil());
}

/*
 * :condition
 * ...
 * jump-if-not condition :right
 * :left
 * ...
 * jump done (only if not tail)
 * :right
 * ...
 * :done
 */
static JanetSlot janetc_if(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetCompiler *c = opts.compiler;
    int32_t labelr, labeljr, labeld, labeljd;
    JanetFopts condopts, bodyopts;
    JanetSlot cond, left, right, target;
    Janet truebody, falsebody;
    JanetScope condscope, tempscope;
    const int tail = opts.flags & JANET_FOPTS_TAIL;
    const int drop = opts.flags & JANET_FOPTS_DROP;

    if (argn < 2 || argn > 3) {
        janetc_cerror(c, "expected 2 or 3 arguments to if");
        return janetc_cslot(janet_wrap_nil());
    }

    /* Get the bodies of the if expression */
    truebody = argv[1];
    falsebody = argn > 2 ? argv[2] : janet_wrap_nil();

    /* Get options */
    condopts = janetc_fopts_default(c);
    bodyopts = opts;

    /* Set target for compilation */
    target = (drop || tail)
        ? janetc_cslot(janet_wrap_nil())
        : janetc_gettarget(opts);

    /* Compile condition */
    janetc_scope(&condscope, c, 0, "if");
    cond = janetc_value(condopts, argv[0]);

    /* Check constant condition. */
    /* TODO: Use type info for more short circuits */
    if (cond.flags & JANET_SLOT_CONSTANT) {
        if (!janet_truthy(cond.constant)) {
            /* Swap the true and false bodies */
            Janet temp = falsebody;
            falsebody = truebody;
            truebody = temp;
        }
        janetc_scope(&tempscope, c, 0, "if-body");
        target = janetc_value(bodyopts, truebody);
        janetc_popscope(c);
        janetc_popscope(c);
        janetc_throwaway(bodyopts, falsebody);
        return target;
    }

    /* Compile jump to right */
    labeljr = janetc_emit_si(c, JOP_JUMP_IF_NOT, cond, 0, 0);

    /* Condition left body */
    janetc_scope(&tempscope, c, 0, "if-true");
    left = janetc_value(bodyopts, truebody);
    if (!drop && !tail) janetc_copy(c, target, left);
    janetc_popscope(c);

    /* Compile jump to done */
    labeljd = janet_v_count(c->buffer);
    if (!tail) janetc_emit(c, JOP_JUMP);

    /* Compile right body */
    labelr = janet_v_count(c->buffer);
    janetc_scope(&tempscope, c, 0, "if-false");
    right = janetc_value(bodyopts, falsebody);
    if (!drop && !tail) janetc_copy(c, target, right);
    janetc_popscope(c);

    /* Pop main scope */
    janetc_popscope(c);

    /* Write jumps - only add jump lengths if jump actually emitted */
    labeld = janet_v_count(c->buffer);
    c->buffer[labeljr] |= (labelr - labeljr) << 16;
    if (!tail) c->buffer[labeljd] |= (labeld - labeljd) << 8;

    if (tail) target.flags |= JANET_SLOT_RETURNED;
    return target;
}

/* Compile a do form. Do forms execute their body sequentially and
 * evaluate to the last expression in the body. */
static JanetSlot janetc_do(JanetFopts opts, int32_t argn, const Janet *argv) {
    int32_t i;
    JanetSlot ret = janetc_cslot(janet_wrap_nil());
    JanetCompiler *c = opts.compiler;
    JanetFopts subopts = janetc_fopts_default(c);
    JanetScope tempscope;
    janetc_scope(&tempscope, c, 0, "do");
    for (i = 0; i < argn; i++) {
        if (i != argn - 1) {
            subopts.flags = JANET_FOPTS_DROP;
        } else {
            subopts = opts;
        }
        ret = janetc_value(subopts, argv[i]);
        if (i != argn - 1) {
            janetc_freeslot(c, ret);
        }
    }
    janetc_popscope_keepslot(c, ret);
    return ret;
}

/* Add a funcdef to the top most function scope */
static int32_t janetc_addfuncdef(JanetCompiler *c, JanetFuncDef *def) {
    JanetScope *scope = c->scope;
    while (scope) {
        if (scope->flags & JANET_SCOPE_FUNCTION)
            break;
        scope = scope->parent;
    }
    janet_assert(scope, "could not add funcdef");
    janet_v_push(scope->defs, def);
    return janet_v_count(scope->defs) - 1;
}

/*
 * :whiletop
 * ...
 * :condition
 * jump-if-not cond :done
 * ...
 * jump :whiletop
 * :done
 */
static JanetSlot janetc_while(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetCompiler *c = opts.compiler;
    JanetSlot cond;
    JanetFopts subopts = janetc_fopts_default(c);
    JanetScope tempscope;
    int32_t labelwt, labeld, labeljt, labelc, i;
    int infinite = 0;

    if (argn < 2) {
        janetc_cerror(c, "expected at least 2 arguments");
        return janetc_cslot(janet_wrap_nil());
    }

    labelwt = janet_v_count(c->buffer);

    janetc_scope(&tempscope, c, 0, "while");

    /* Compile condition */
    cond = janetc_value(subopts, argv[0]);

    /* Check for constant condition */
    if (cond.flags & JANET_SLOT_CONSTANT) {
        /* Loop never executes */
        if (!janet_truthy(cond.constant)) {
            janetc_popscope(c);
            return janetc_cslot(janet_wrap_nil());
        }
        /* Infinite loop */
        infinite = 1;
    }

    /* Infinite loop does not need to check condition */
    labelc = infinite
        ? 0
        : janetc_emit_si(c, JOP_JUMP_IF_NOT, cond, 0, 0);

    /* Compile body */
    for (i = 1; i < argn; i++) {
        subopts.flags = JANET_FOPTS_DROP;
        janetc_freeslot(c, janetc_value(subopts, argv[i]));
    }

    /* Check if closure created in while scope. If so,
     * recompile in a function scope. */
    if (tempscope.flags & JANET_SCOPE_CLOSURE) {
        tempscope.flags |= JANET_SCOPE_UNUSED;
        janetc_popscope(c);
        janet_v__cnt(c->buffer) = labelwt;
        janet_v__cnt(c->mapbuffer) = labelwt;

        janetc_scope(&tempscope, c, JANET_SCOPE_FUNCTION, "while-iife");

        /* Recompile in the function scope */
        cond = janetc_value(subopts, argv[0]);
        if (!(cond.flags & JANET_SLOT_CONSTANT)) {
            /* If not an infinte loop, return nil when condition false */
            janetc_emit_si(c, JOP_JUMP_IF, cond, 2, 0);
            janetc_emit(c, JOP_RETURN_NIL);
        }
        for (i = 1; i < argn; i++) {
            subopts.flags = JANET_FOPTS_DROP;
            janetc_freeslot(c, janetc_value(subopts, argv[i]));
        }
        /* But now add tail recursion */
        int32_t tempself = janetc_regalloc_temp(&tempscope.ra, JANETC_REGTEMP_0);
        janetc_emit(c, JOP_LOAD_SELF | (tempself << 8));
        janetc_emit(c, JOP_TAILCALL | (tempself << 8));
        /* Compile function */
        JanetFuncDef *def = janetc_pop_funcdef(c);
        def->name = janet_cstring("_while");
        int32_t defindex = janetc_addfuncdef(c, def);
        /* And then load the closure and call it. */
        int32_t cloreg = janetc_regalloc_temp(&c->scope->ra, JANETC_REGTEMP_0);
        janetc_emit(c, JOP_CLOSURE | (cloreg << 8) | (defindex << 16));
        janetc_emit(c, JOP_CALL | (cloreg << 8) | (cloreg << 16));
        janetc_regalloc_free(&c->scope->ra, cloreg);
        c->scope->flags |= JANET_SCOPE_CLOSURE;
        return janetc_cslot(janet_wrap_nil());
    }

    /* Compile jump to whiletop */
    labeljt = janet_v_count(c->buffer);
    janetc_emit(c, JOP_JUMP);

    /* Calculate jumps */
    labeld = janet_v_count(c->buffer);
    if (!infinite) c->buffer[labelc] |= (labeld - labelc) << 16;
    c->buffer[labeljt] |= (labelwt - labeljt) << 8;

    /* Pop scope and return nil slot */
    janetc_popscope(c);

    return janetc_cslot(janet_wrap_nil());
}

static JanetSlot janetc_fn(JanetFopts opts, int32_t argn, const Janet *argv) {
    JanetCompiler *c = opts.compiler;
    JanetFuncDef *def;
    JanetSlot ret;
    Janet head, paramv;
    JanetScope fnscope;
    int32_t paramcount, argi, parami, arity, defindex;
    JanetFopts subopts = janetc_fopts_default(c);
    const Janet *params;
    const char *errmsg = NULL;
    int varargs = 0;
    int selfref = 0;

    /* Begin function */
    c->scope->flags |= JANET_SCOPE_CLOSURE;
    janetc_scope(&fnscope, c, JANET_SCOPE_FUNCTION, "function");

    if (argn < 2) {
        errmsg = "expected at least 2 arguments to function literal";
        goto error;
    }

    /* Read function parameters */
    parami = 0;
    arity = 0;
    head = argv[0];
    if (janet_checktype(head, JANET_SYMBOL)) {
        selfref = 1;
        parami = 1;
    }
    if (parami >= argn) {
        errmsg = "expected function parameters";
        goto error;
    }
    paramv = argv[parami];
    if (janet_indexed_view(paramv, &params, &paramcount)) {
        int32_t i;
        for (i = 0; i < paramcount; i++) {
            Janet param = params[i];
            if (janet_checktype(param, JANET_SYMBOL)) {
                /* Check for varargs */
                if (0 == janet_cstrcmp(janet_unwrap_symbol(param), "&")) {
                    if (i != paramcount - 2) {
                        errmsg = "variable argument symbol in unexpected location";
                        goto error;
                    }
                    varargs = 1;
                    arity--;
                    continue;
                }
                janetc_nameslot(c, janet_unwrap_symbol(param), janetc_farslot(c));
            } else {
                destructure(c, param, janetc_farslot(c), defleaf, NULL);
            }
            arity++;
        }
    } else {
        errmsg = "expected function parameters";
        goto error;
    }

    /* Check for self ref */
    if (selfref) {
        JanetSlot slot = janetc_farslot(c);
        slot.flags = JANET_SLOT_NAMED | JANET_FUNCTION;
        janetc_emit_s(c, JOP_LOAD_SELF, slot, 1);
        janetc_nameslot(c, janet_unwrap_symbol(head), slot);
    }

    /* Compile function body */
    if (parami + 1 == argn) {
        janetc_emit(c, JOP_RETURN_NIL);
    } else for (argi = parami + 1; argi < argn; argi++) {
        subopts.flags = (argi == (argn - 1)) ? JANET_FOPTS_TAIL : JANET_FOPTS_DROP;
        janetc_value(subopts, argv[argi]);
        if (c->result.status == JANET_COMPILE_ERROR)
            goto error2;
    }

    /* Build function */
    def = janetc_pop_funcdef(c);
    def->arity = arity;

    /* Tuples indicated fixed arity, arrays indicate flexible arity */
    /* TODO - revisit this */
    if (varargs) 
        def->flags |= JANET_FUNCDEF_FLAG_VARARG;
    else if (janet_checktype(paramv, JANET_TUPLE))
        def->flags |= JANET_FUNCDEF_FLAG_FIXARITY;

    if (selfref) def->name = janet_unwrap_symbol(head);
    defindex = janetc_addfuncdef(c, def);

    /* Ensure enough slots for vararg function. */
    if (arity + varargs > def->slotcount) def->slotcount = arity + varargs;

    /* Instantiate closure */
    ret = janetc_gettarget(opts);
    janetc_emit_su(c, JOP_CLOSURE, ret, defindex, 1);
    return ret;

error:
    janetc_cerror(c, errmsg);
error2:
    janetc_popscope(c);
    return janetc_cslot(janet_wrap_nil());
}

/* Keep in lexicographic order */
static const JanetSpecial janetc_specials[] = {
    {":=", janetc_varset},
    {"def", janetc_def},
    {"do", janetc_do},
    {"fn", janetc_fn},
    {"if", janetc_if},
    {"quote", janetc_quote},
    {"var", janetc_var},
    {"while", janetc_while}
};

/* Find a special */
const JanetSpecial *janetc_special(const uint8_t *name) {
    return janet_strbinsearch(
            &janetc_specials,
            sizeof(janetc_specials)/sizeof(JanetSpecial),
            sizeof(JanetSpecial),
            name);
}

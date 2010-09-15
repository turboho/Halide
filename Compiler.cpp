#include "Compiler.h"


// Compile a gather
void Compiler::compileGather(AsmX64 *a, FImage *im) {
    // Only consider the first definition for now
    LVal def = im->definitions[0];
    IRNode::Ptr root = def.node;
    
    // Force the output type of the expression to be a float
    root = root->as(IRNode::Float);

    // Assign loop levels 
    def.x.node->assignLevel(2);
    def.y.node->assignLevel(1);

    printf("Compiling: ");
    root->printExp();
    printf("\n");

    // vectorize across X
    // TODO: right now it's hacked in
    // root = root->vectorize(def.x.node);

    IRNode::saveDot("before.dot");

    printf("Unrolling...\n");    
    // Unroll across C and reoptimize
    vector<IRNode::Ptr > roots(im->channels); 
    for (int i = 0; i < im->channels; i++) {
        roots[i] = root->substitute(def.c.node, IRNode::make(i));
    }
    printf("Done\n");

    IRNode::saveDot("after.dot");


    // Assign the variables some registers
    AsmX64::Reg x = a->rax, y = a->rcx, 
        tmp = a->r15, outPtr = a->rbp;

    // Mark these registers as unclobberable for the register allocation
    uint32_t reserved = ((1 << x.num) |
                         (1 << y.num) |
                         (1 << outPtr.num) |
                         (1 << tmp.num) | 
                         (1 << a->rsp.num));

    // Force the vars into the intended registers
    def.x.node->reg = x.num;
    def.y.node->reg = y.num;

    // Register assignment and evaluation ordering. This returns a
    // vector of vector of IRNode - one to be computed at each loop
    // level. We're assuming the loop structure looks like this:

    // compute constants (order[0])
    // for var level 1:
    //   compute things that depend on var level 1 (order[1])
    //   for var level 2:
    //     compute things that depend on var level 2 (order[2])
    //     for var level 3:
    //       compute things that depend on var level 3 (order[3])
    //       for ...
    //          ...

    printf("Register assignment...\n");
    vector<uint32_t> clobbered, outputs;
    vector<vector<IRNode::Ptr > > order;        
    doRegisterAssignment(roots, reserved, order, clobbered, outputs);   
    printf("Done\n");

    // This should have resulted in three loop levels
    assert(order.size() == 3, 
           "I was expecting four loop levels (constants, y, x)");

    // print out the proposed ordering and register assignment for inspection
    const char *dims = "yx";
    const int range[2][2] = {{def.y.min, def.y.max},
                             {def.x.min, def.x.max}};
    for (size_t l = 0; l < order.size(); l++) {
        if (l) {
            for (size_t k = 1; k < l; k++) putchar(' ');
            printf("for %c from %d to %d:\n",
                   dims[l-1], range[l-1][0], range[l-1][1]);
        }
        for (size_t i = 0; i < order[l].size(); i++) {
            IRNode::Ptr next = order[l][i];
            for (size_t k = 0; k < l; k++) putchar(' ');
            next->print();
        }
    }
        
    // Align the stack to a 16-byte boundary - it always comes in
    // offset by 8 bytes because it contains the 64-bit return
    // address.
    a->sub(a->rsp, 8);
        
    // Save all the registers that the 64-bit C abi tells us we're
    // supposed to. This maintains stack alignment.
    a->pushNonVolatiles();
        
    // Generate constants
    compileBody(a, order[0]);
    a->mov(y, def.y.min);
    a->label("yloop"); 
        
    // Compute the address of the start of this scanline in the output
    a->mov(outPtr, im->data);           
    a->mov(tmp, y);
    a->imul(tmp, im->width*im->channels*sizeof(float));
    a->add(outPtr, tmp);
    a->add(outPtr, def.x.min*im->channels*sizeof(float));
        
    // Generate the values that don't depend on X
    compileBody(a, order[1]);        
    a->mov(x, def.x.min);               
    a->label("xloop"); 
        
    // Add a mark for the intel static analyzer tool
    // a->iacaStart();

    // Generate the nodes that compute the roots
    compileBody(a, order[2]);       
       
    // Transpose and store a block of data. Only works for 3-channels right now.
    if (im->channels == 3) {
        AsmX64::SSEReg tmps[] = {roots[0]->reg, roots[1]->reg, roots[2]->reg,
                                 a->xmm14, a->xmm15};
        // tmp0 = r0, r1, r2, r3
        // tmp1 = g0, g1, g2, g3
        // tmp2 = b0, b1, b2, b3
        a->movaps(tmps[3], tmps[0]);
        // tmp3 = r0, r1, r2, r3
        a->shufps(tmps[3], tmps[2], 1, 3, 0, 2);            
        // tmp3 = r1, r3, b0, b2
        a->movaps(tmps[4], tmps[1]);
        // tmp4 = g0, g1, g2, g3
        a->shufps(tmps[4], tmps[2], 1, 3, 1, 3);
        // tmp4 = g1, g3, b1, b3
        a->shufps(tmps[0], tmps[1], 0, 2, 0, 2);
        // tmp0 = r0, r2, g0, g2
        a->movaps(tmps[1], tmps[0]);
        // tmp1 = r0, r2, g0, g2
        a->shufps(tmps[1], tmps[3], 0, 2, 2, 0);
        // tmp1 = r0, g0, b0, r1
        a->movntps(AsmX64::Mem(outPtr), tmps[1]);
        a->movaps(tmps[1], tmps[4]);
        // tmp1 = g1, g3, b1, b3            
        a->shufps(tmps[1], tmps[0], 0, 2, 1, 3);
        // tmp1 = g1, b1, r2, g2
        a->movntps(AsmX64::Mem(outPtr, 16), tmps[1]);
        a->movaps(tmps[1], tmps[3]);
        // tmp1 = r1, r3, b0, b2
        a->shufps(tmps[1], tmps[4], 3, 1, 1, 3);
        // tmp1 = b2, r3, g3, b3
        a->movntps(AsmX64::Mem(outPtr, 32), tmps[1]);
    } else {
        panic("For now I can't deal with images with channel counts other than 3\n");
    }
        
    // Move on to the next X (actually jump by four due to the hacked vectorization we're doing)
    a->add(outPtr, im->channels*4*4);
    a->add(x, 4);
    a->cmp(x, def.x.max);
    a->jl("xloop");

    // add a mark for the intel static analyzer
    //a->iacaEnd();

    // Next y
    a->add(y, 1);
    a->cmp(y, def.y.max);
    a->jl("yloop");            

    // Pop the stack and return
    a->popNonVolatiles();
    a->add(a->rsp, 8);
    a->ret();        

    // Save an object file that you can use dumpbin on to inspect
    a->saveCOFF("generated.obj");        
}
    
// Generate machine code for a vector of IRNodes. Registers must have already been assigned.
void Compiler::compileBody(AsmX64 *a, vector<IRNode::Ptr > code) {
        
    AsmX64::SSEReg tmp2 = a->xmm14;
    AsmX64::SSEReg tmp = a->xmm15;
    AsmX64::Reg gtmp = a->r15;

    for (size_t i = 0; i < code.size(); i++) {
        // Extract the node, its register, and any inputs and their registers
        IRNode::Ptr node = code[i];
        IRNode::Ptr c1 = (node->inputs.size() >= 1) ? node->inputs[0] : NULL;
        IRNode::Ptr c2 = (node->inputs.size() >= 2) ? node->inputs[1] : NULL;
        IRNode::Ptr c3 = (node->inputs.size() >= 3) ? node->inputs[2] : NULL;
        IRNode::Ptr c4 = (node->inputs.size() >= 4) ? node->inputs[3] : NULL;

        // SSE source and destination registers
        AsmX64::SSEReg dst(node->reg-16);
        AsmX64::SSEReg src1(c1 ? c1->reg-16 : 0);
        AsmX64::SSEReg src2(c2 ? c2->reg-16 : 0);
        AsmX64::SSEReg src3(c3 ? c3->reg-16 : 0);
        AsmX64::SSEReg src4(c4 ? c4->reg-16 : 0);

        // Is the destination a GPR?
        bool gpr = node->reg < 16;

        // Which sources are GPRs?
        bool gpr1 = c1 && c1->reg < 16;
        bool gpr2 = c2 && c2->reg < 16;
        bool gpr3 = c3 && c3->reg < 16;
        bool gpr4 = c4 && c4->reg < 16;

        // GPR source and destination registers
        AsmX64::Reg gdst(node->reg);
        AsmX64::Reg gsrc1(c1 ? c1->reg : 0);
        AsmX64::Reg gsrc2(c2 ? c2->reg : 0);
        AsmX64::Reg gsrc3(c3 ? c3->reg : 0);
        AsmX64::Reg gsrc4(c4 ? c4->reg : 0);

        switch(node->op) {
        case Const: 
            if (node->type == IRNode::Float) {
                if (node->fval == 0.0f) {
                    a->bxorps(dst, dst);
                } else {
                    a->mov(gtmp, &(node->fval));
                    a->movss(dst, AsmX64::Mem(gtmp));
                    a->shufps(dst, dst, 0, 0, 0, 0);
                }
            } else if (node->type == IRNode::Bool) {
                if (gpr) {
                    if (node->ival) 
                        a->mov(gdst, -1);
                    else
                        a->mov(gdst, 0);
                } else {
                    if (node->ival) {
                        a->cmpeqps(dst, dst);
                    } else {
                        a->bxorps(dst, dst);                    
                    }
                }
            } else {
                if (gpr) {
                    a->mov(gdst, node->ival);
                } else {
                    a->mov(a->r15, node->ival);
                    // ints are 32-bit for now, so this works
                    a->cvtsi2ss(dst, a->r15);
                    a->shufps(dst, dst, 0, 0, 0, 0);                        
                }
            }
            break;
        case Var:
            // These are placed in GPRs externally
            assert(gpr, "Vars must be manually placed in gprs\n");
            break;
        case Plus:
            if (gpr && gpr1 && gpr2) {
                if (gdst == gsrc1)
                    a->add(gdst, gsrc2);
                else if (gdst == gsrc2) 
                    a->add(gdst, gsrc1);
                else {
                    a->mov(gdst, gsrc1);
                    a->add(gdst, gsrc2);
                }
            } else if (!gpr && !gpr1 && !gpr2) {
                if (dst == src1)
                    a->addps(dst, src2);
                else if (dst == src2) 
                    a->addps(dst, src1);
                else {
                    a->movaps(dst, src1);
                    a->addps(dst, src2);
                }
            } else {
                panic("Can't add between gpr/sse\n");
            }
            break;
        case Minus:
            if (gpr && gpr1 && gpr2) {
                if (gdst == gsrc1) {
                    a->sub(gdst, gsrc2);
                } else if (gdst == gsrc2) {
                    a->mov(gtmp, gsrc2);
                    a->mov(gsrc2, gsrc1);
                    a->sub(gsrc2, gtmp);
                } else {                         
                    a->mov(gdst, gsrc1);
                    a->sub(gdst, gsrc2);
                }
            } else if (!gpr && !gpr1 && !gpr2) {
                if (dst == src1) {
                    a->subps(dst, src2);
                } else if (dst == src2) {
                    a->movaps(tmp, src2);
                    a->movaps(src2, src1);
                    a->subps(src2, tmp);
                } else { 
                    a->movaps(dst, src1);
                    a->subps(dst, src2);
                }
            } else {
                panic("Can't sub between gpr/sse\n");
            }
            break;
        case Times: 
            if (gpr && gpr1 && gpr2) {
                if (gdst == gsrc1)
                    a->imul(gdst, gsrc2);
                else if (gdst == gsrc2) 
                    a->imul(gdst, gsrc1);
                else {
                    a->mov(gdst, gsrc1);
                    a->imul(gdst, gsrc2);
                }
            } else if (!gpr && !gpr1 && !gpr2) {
                if (dst == src1)
                    a->mulps(dst, src2);
                else if (dst == src2) 
                    a->mulps(dst, src1);
                else {
                    a->movaps(dst, src1);
                    a->mulps(dst, src2);
                }
            } else {
                panic("Can't sub between gpr/sse\n");
            }
            break;
        case TimesImm:
            if (gdst == gsrc1) {
                a->imul(gdst, node->ival);
            } else {
                a->mov(gdst, node->ival);
                a->imul(gdst, gsrc1);
            }
            break;
        case PlusImm:
            if (gdst == gsrc1) {
                a->add(gdst, node->ival);
            } else {
                a->mov(gdst, node->ival);
                a->add(gdst, gsrc1);
            }
            break;
        case Divide: 
            assert(!gpr && !gpr1 && !gpr2, "Can only divide in sse regs for now\n");
            if (dst == src1) {
                a->divps(dst, src2);
            } else if (dst == src2) {
                a->movaps(tmp, src2);
                a->movaps(src2, src1);
                a->divps(src2, tmp); 
            } else {
                a->movaps(dst, src1);
                a->divps(dst, src2);
            }
            break;
        case And:
            assert(!gpr && !gpr1 && !gpr2, "Can only and in sse regs for now\n");
            if (dst == src1) 
                a->bandps(dst, src2);
            else if (dst == src2)
                a->bandps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->bandps(dst, src2);
            }
            break;
        case Nand:
            assert(!gpr && !gpr1 && !gpr2, "Can only nand in sse regs for now\n");
            if (dst == src1) {
                a->bandnps(dst, src2);
            } else if (dst == src2) {
                a->movaps(tmp, src2);
                a->movaps(src2, src1);
                a->bandnps(src2, tmp); 
            } else {
                a->movaps(dst, src1);
                a->bandnps(dst, src2);
            }
            break;
        case Or:               
            assert(!gpr && !gpr1 && !gpr2, "Can only or in sse regs for now\n");
            if (dst == src1) 
                a->borps(dst, src2);
            else if (dst == src2)
                a->borps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->borps(dst, src2);
            }
            break;
        case NEQ:                               
            assert(!gpr && !gpr1 && !gpr2, "Can only neq in sse regs for now\n");
            if (dst == src1) 
                a->cmpneqps(dst, src2);
            else if (dst == src2)
                a->cmpneqps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpneqps(dst, src2);
            }
            break;
        case EQ:
            assert(!gpr && !gpr1 && !gpr2, "Can only eq in sse regs for now\n");
            if (dst == src1) 
                a->cmpeqps(dst, src2);
            else if (dst == src2)
                a->cmpeqps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpeqps(dst, src2);
            }
            break;
        case LT:
            assert(!gpr && !gpr1 && !gpr2, "Can only lt in sse regs for now\n");
            if (dst == src1) 
                a->cmpltps(dst, src2);
            else if (dst == src2)
                a->cmpnleps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpltps(dst, src2);
            }
            break;
        case GT:
            assert(!gpr && !gpr1 && !gpr2, "Can only gt in sse regs for now\n");
            if (dst == src1) 
                a->cmpnleps(dst, src2);
            else if (dst == src2)
                a->cmpltps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpnleps(dst, src2);
            }
            break;
        case LTE:
            assert(!gpr && !gpr1 && !gpr2, "Can only lte in sse regs for now\n");
            if (dst == src1) 
                a->cmpleps(dst, src2);
            else if (dst == src2)
                a->cmpnltps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpleps(dst, src2);
            }
            break;
        case GTE:
            assert(!gpr && !gpr1 && !gpr2, "Can only gte in sse regs for now\n");
            if (dst == src1) 
                a->cmpnltps(dst, src2);
            else if (dst == src2)
                a->cmpleps(dst, src1);
            else {
                a->movaps(dst, src1);
                a->cmpnltps(dst, src2);
            }
            break;
        case ATan2:
        case Mod:
        case Power:
        case Sin:
        case Cos:
        case Tan:
        case ASin:
        case ACos:
        case ATan:
        case Exp:
        case Log:
        case Floor:
        case Ceil:
        case Round:
        case Abs:               
        case FloatToInt:
            panic("Not implemented: %s\n", opname[node->op]);                
            break;
        case IntToFloat:
            if (gpr1 && !gpr) {
                a->cvtsi2ss(dst, gsrc1);
                a->shufps(dst, dst, 0, 0, 0, 0);
            } else {
                panic("IntToFloat can only go from gpr to sse\n");
            }
            break;
        case Load:
            node->ival = 0;
        case LoadImm:
            assert(gpr1, "Can only load using addresses in gprs\n");
            assert(!gpr, "Can only load into sse regs\n");
            a->movss(dst, AsmX64::Mem(gsrc1, node->ival));
            a->movss(tmp, AsmX64::Mem(gsrc1, node->ival + 3*4));
            a->punpckldq(dst, tmp);
            a->movss(tmp, AsmX64::Mem(gsrc1, node->ival + 3*8));
            a->movss(tmp2, AsmX64::Mem(gsrc1, node->ival + 3*12));
            a->punpckldq(tmp, tmp2);
            a->punpcklqdq(dst, tmp);
               

        case NoOp:
            break;
        }
    }
}

// This function assigns registers and generates an evaluation
// order for an array of expressions (roots). It takes as input:
//
// roots: the vector of expressions to be assigned registers
//
// reserved: Registers corresponding to bits marked high in this
// mask may not be used. Bits 30 and 31 (which correspond to
// registers xmm14 and xmm15) may not be marked high, because the
// code generator uses those as scratch.
// 
// As output it returns primarily order: five arrays of IRNodes,
// one to be computed at each loop level. We're assuming the loop
// structure looks like this:
//
// compute constants (order[0])
// for t:
//   compute things that depend on t (order[1])
//   for y:
//     compute things that depend on y (order[2])
//     for x:
//       compute things that depend on x (order[3])
//       for c:
//         compute things that depens on c (order[4])
// 
// Also, clobberedRegs will contain masks of which registers get
// clobbered at each level, and outputRegs will indicate which
// registers contain output from a level (i.e. registers either
// used by roots, or used by a higher level).
// 
void Compiler::doRegisterAssignment(
    const vector<IRNode::Ptr > &roots, 
    uint32_t reserved,
    vector<vector<IRNode::Ptr > > &order,
    vector<uint32_t> &clobberedRegs, 
    vector<uint32_t> &outputRegs) {


    // Who's currently occupying which register? First the 16 gprs, then the 16 sse registers.
    vector<IRNode::Ptr > regs(32);

    // Reserve xmm14-15 for the code generator to use as scratch
    assert(!(reserved & ((1<<30) | (1<<31))), 
           "Registers xmm14 and xmm15 are reserved for the code generator\n");
    reserved |= (1<<30) | (1<<31);
        
    // Clear any previous register assignment and order
    for (size_t i = 0; i < roots.size(); i++) {
        regClear(roots[i]);
    }
    order.clear();

    for (size_t i = 0; i < roots.size(); i++) {
        // Assign registers to this expression
        regAssign(roots[i], reserved, regs, order);
        // Don't let the next expression clobber the output of the
        // previous expressions
        reserved |= (1 << roots[i]->reg);
    }

    // Detect what registers get clobbered
    clobberedRegs.clear();
    clobberedRegs.resize(order.size(), 0);
    for (int i = 0; i < order.size(); i++) {
        clobberedRegs[i] = (1<<30) | (1<<31);
        for (size_t j = 0; j < order[i].size(); j++) {
            IRNode::Ptr node = order[i][j];
            clobberedRegs[i] |= (1 << node->reg);
        }
    }


    // Detect what registers are used for inter-level communication
    outputRegs.clear();
    outputRegs.resize(order.size(), 0);
    
    if (outputRegs.size()) outputRegs[0] = 0;
    for (int i = 1; i < order.size(); i++) {
        outputRegs[i] = 0;
        for (size_t j = 0; j < order[i].size(); j++) {
            IRNode::Ptr node = order[i][j];
            for (size_t k = 0; k < node->inputs.size(); k++) {
                IRNode::Ptr input = node->inputs[k];
                if (input->level != node->level) {
                    outputRegs[input->level] |= (1 << input->reg);
                }
            }
        }
    }    


    // Detect what registers are used as the final outputs
    for (size_t i = 0; i < roots.size(); i++) {
        outputRegs[outputRegs.size()-1] |= (1 << roots[i]->reg);
    }

    for (size_t i = 0; i < order.size(); i++) {
        printf("Outputs at level %d\n", i);
        for (int j = 0; j < 32; j++) {
            if (outputRegs[i] & (1<<j)) printf("%d ", j);
        }
        printf("\n");
        printf("Clobbers at level %d\n", i);
        for (int j = 0; j < 32; j++) {
            if (clobberedRegs[i] & (1<<j)) printf("%d ", j);
        }
        printf("\n");
    }

}

// Remove all assigned registers
void Compiler::regClear(IRNode::Ptr node) {
    // We don't clobber the registers assigned to external loop vars
    if (node->op == Var) return;

    node->reg = -1;
    for (size_t i = 0; i < node->inputs.size(); i++) {
        regClear(node->inputs[i]);
    }        
}
     
// Recursively assign registers to sub-expressions
void Compiler::regAssign(IRNode::Ptr node,
                         uint32_t reserved,
                         vector<IRNode::Ptr > &regs, 
                         vector<vector<IRNode::Ptr > > &order) {

    // Check we're at a known loop level
    assert(node->level || node->constant, "Cannot assign registers to a node that depends on a variable with a loop order not yet assigned.\n");

    // Expand the order vector as necessary if we discover deeper loop levels exist
    if (order.size() <= node->level) order.resize(node->level+1);

    // If I already have a register bail out. This may occur
    // because I was manually assigned a register outside
    // doRegisterAssignment, or if common subexpression
    // elimination has resulted in two parts of the expression
    // pointing to the same IRNode.
    if (node->reg >= 0) {
        return;
    }

    // Recursively assign registers to the inputs
    for (size_t i = 0; i < node->inputs.size(); i++) {
        regAssign(node->inputs[i], reserved, regs, order);
    }

    // Figure out if we're going into a GPR or an SSE
    // register. All vectors go in SSE. Scalar floats also go in
    // SSE.
    bool gpr = (node->width == 1) && (node->type != IRNode::Float);

    // If there are inputs, see if we can use the register of the
    // one of the inputs as output - the first is optimal, as this
    // makes x64 codegen easier. To reuse the register of the
    // input it has to be at the same level as us (otherwise it
    // will have been computed once and stored outside the for
    // loop this node lives in), and have no other outputs that
    // haven't already been evaluated.
    if (node->inputs.size()) {

        IRNode::Ptr input1 = node->inputs[0];
        bool okToClobber = true;

        // Check it's not reserved.
        if (reserved & (1 << input1->reg)) okToClobber = false;

        // Check it's the same type of register.
        if ((gpr && (input1->reg >= 16)) ||
            (!gpr && (input1->reg < 16))) okToClobber = false;

        // Must be at the same loop level.
        if (node->level != input1->level) okToClobber = false;
 
        // Every parent must be this node, or at the same level
        // and already evaluated. Note that a parent can't
        // possible be at a higher loop level.
        for (size_t i = 0; i < input1->outputs.size() && okToClobber; i++) {
            IRNode::Ptr out = input1->outputs[i].lock();
            if (!out) continue;
            if (out != node && 
                (out->level != node->level ||
                 out->reg < 0)) {
                okToClobber = false;
            }
        }
        if (okToClobber) {
            node->reg = input1->reg;
            regs[input1->reg] = node;
            order[node->level].push_back(node);
            return;
        }
    }
        
    // Some binary ops are easy to flip, so we should try to
    // clobber the second input next for those.

    if (node->op == And ||
        node->op == Or ||
        node->op == Plus ||
        node->op == Times ||
        node->op == LT ||
        node->op == GT ||
        node->op == LTE ||
        node->op == GTE ||
        node->op == EQ ||
        node->op == NEQ) {

        IRNode::Ptr input2 = node->inputs[1];
        bool okToClobber = true;

        // Check it's not reserved.
        if (reserved & (1 << input2->reg)) okToClobber = false;

        // Check it's the same type of register.
        if ((gpr && (input2->reg >= 16)) ||
            (!gpr && (input2->reg < 16))) okToClobber = false;

        // Must be the same level.
        if (node->level != input2->level) okToClobber = false;

        // Every parent must be this, or at the same level and already evaluated.
        for (size_t i = 0; i < input2->outputs.size() && okToClobber; i++) {
            IRNode::Ptr out = input2->outputs[i].lock();
            if (!out) continue;
            if (out != node && 
                (out->level != node->level ||
                 out->reg < 0)) {
                okToClobber = false;
            }
        }
        if (okToClobber) {
            node->reg = input2->reg;
            regs[input2->reg] = node;
            order[node->level].push_back(node);
            return;
        }
    }


    // Next, try to find a previously used register that is safe
    // to clobber - meaning it's at the same or higher level and all
    // its outputs will have already been evaluated and are at the
    // same or higher level.
    for (size_t i = 0; i < regs.size(); i++) {            
        // Don't consider unused registers yet
        if (!regs[i]) continue;

        // Check it's not reserved
        if (reserved & (1 << i)) continue;

        // Check it's the right type of register
        if (gpr && (i >= 16)) break;
        if (!gpr && (i < 16)) continue;

        // Don't clobber registers from a higher level
        if (regs[i]->level < node->level) {
            printf("The occupant of %d is at level %d, whereas I'm at level %d\n", i, regs[i]->level, node->level);
        }

        // Only clobber registers whose outputs will have been
        // fully evaluated - if they've already been assigned
        // registers that means they're ahead of us in the
        // evaluation order and will have already been
        // evaluated.
        bool safeToEvict = true;            
        for (size_t j = 0; j < regs[i]->outputs.size(); j++) {
            IRNode::Ptr out = regs[i]->outputs[j].lock();
            if (!out) continue;
            if (out->reg < 0 ||
                out->level > node->level) {
                safeToEvict = false;
                break;
            }
        }

        if (safeToEvict) {
            node->reg = i;
            regs[i] = node;
            order[node->level].push_back(node);
            return;
        } else {
            printf("The occupant of %d has an unevaluated output still to come\n", i);
        }
    }


    // Find a completely unused register and use that. 
    for (size_t i = 0; i < regs.size(); i++) {
        // Don't consider used registers
        if (regs[i]) continue;
            
        // Don't consider the wrong type of register
        if (gpr && (i >= 16)) break;
        if (!gpr && (i < 16)) continue;

        // Don't consider reserved registers
        if (reserved & (1 << i)) continue;

        node->reg = i;
        regs[i] = node;
        order[node->level].push_back(node);
        return;
    }

    // Finally, clobber a non-primary input. This sometimes
    // requires two inserted movs, so it's the least favored
    // option.
    for (size_t i = 1; i < node->inputs.size(); i++) {
        IRNode::Ptr input = node->inputs[i];

        bool okToClobber = true;

        // Check it's not reserved
        if (reserved & (1 << input->reg)) okToClobber = false;

        // Check it's the same type of register
        if ((gpr && (input->reg >= 16)) ||
            (!gpr && (input->reg < 16))) okToClobber = false;

        // Must be the same level
        if (node->level != input->level) okToClobber = false;

        // Every parent must be this node, or at the same level and already evaluated
        for (size_t i = 0; i < input->outputs.size() && okToClobber; i++) {
            IRNode::Ptr out = input->outputs[i].lock();
            if (!out) continue;
            if (out != node && 
                (out->level != node->level ||
                 out->reg < 0)) {
                okToClobber = false;
            }
        }
        if (okToClobber) {
            node->reg = input->reg;
            regs[input->reg] = node;
            order[node->level].push_back(node);
            return;
        }
    }
        
    // Freak out - we're out of registers and we don't know
    // how to spill to the stack yet. 
    printf("Register assignments:\n");
    for (size_t i = 0; i < regs.size(); i++) {
        if (regs[i]) {
            printf("%d: ", i, opname[regs[i]->op]);
            regs[i]->printExp();
            printf("\n");
        } else if (reserved & (1<<i)) 
            printf("%d: (reserved)\n", i);
        else
            printf("%d: (empty)\n", i);
    }
    panic("Out of registers compiling %s!\n", opname[node->op]);
}
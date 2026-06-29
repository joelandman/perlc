#include "ast.h"

NodePtr Node::clone() const {
    auto n = std::make_unique<Node>();
    n->kind = kind;
    n->line = line;
    n->ival = ival;
    n->fval = fval;
    n->sval = sval;
    n->name = name;
    if (left)  n->left  = left->clone();
    if (right) n->right = right->clone();
    if (cond)  n->cond  = cond->clone();
    if (body)  n->body  = body->clone();
    if (init)  n->init  = init->clone();
    if (step)  n->step  = step->clone();
    for (auto &a : args) n->args.push_back(a->clone());
    for (const auto &b : branches) {
        IfBranch nb;
        nb.cond = b.cond ? b.cond->clone() : nullptr;
        nb.body = b.body ? b.body->clone() : nullptr;
        n->branches.push_back(std::move(nb));
    }
    n->params = params;
    return n;
}

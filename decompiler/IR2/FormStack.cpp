#include <algorithm>
#include "FormStack.h"
#include "Form.h"

namespace decompiler {
std::string FormStack::StackEntry::print(const Env& env) const {
  if (destination.has_value()) {
    assert(source && !elt);
    return fmt::format("d: {} s: {} | {} <- {} f: {}", active, sequence_point,
                       destination.value().reg().to_charp(), source->to_string(env),
                       non_seq_source.has_value());
  } else {
    assert(elt && !source);
    return fmt::format("d: {} s: {} | {} f: {}", active, sequence_point, elt->to_string(env),
                       non_seq_source.has_value());
  }
}

std::string FormStack::print(const Env& env) {
  std::string result;
  for (auto& x : m_stack) {
    result += x.print(env);
    result += '\n';
  }
  return result;
}

void FormStack::push_value_to_reg(Variable var,
                                  Form* value,
                                  bool sequence_point,
                                  const SetVarInfo& info) {
  assert(value);
  StackEntry entry;
  entry.active = true;  // by default, we should display everything!
  entry.sequence_point = sequence_point;
  entry.destination = var;
  entry.source = value;
  entry.set_info = info;
  m_stack.push_back(entry);
}

void FormStack::push_value_to_reg_dead(Variable var,
                                       Form* value,
                                       bool sequence_point,
                                       const SetVarInfo& info) {
  assert(value);
  StackEntry entry;
  entry.active = false;
  entry.sequence_point = sequence_point;
  entry.destination = var;
  entry.source = value;
  entry.set_info = info;
  m_stack.push_back(entry);
}

void FormStack::push_non_seq_reg_to_reg(const Variable& dst,
                                        const Variable& src,
                                        Form* src_as_form,
                                        const SetVarInfo& info) {
  assert(src_as_form);
  StackEntry entry;
  entry.active = true;
  entry.sequence_point = false;
  entry.destination = dst;
  entry.non_seq_source = src;
  entry.source = src_as_form;
  entry.set_info = info;
  m_stack.push_back(entry);
}

bool FormStack::is_single_expression() {
  int count = 0;
  for (auto& e : m_stack) {
    if (e.active) {
      count++;
    }
  }
  return count == 1;
}

void FormStack::push_form_element(FormElement* elt, bool sequence_point) {
  StackEntry entry;
  entry.active = true;
  entry.elt = elt;
  entry.sequence_point = sequence_point;
  m_stack.push_back(entry);
}

Form* FormStack::pop_reg(const Variable& var,
                         const RegSet& barrier,
                         const Env& env,
                         bool allow_side_effects,
                         int begin_idx) {
  return pop_reg(var.reg(), barrier, env, allow_side_effects, begin_idx);
}

namespace {
bool nonempty_intersection(const RegSet& a, const RegSet& b) {
  // todo - if we ever switch to bit reg sets, this could be a lot faster.
  std::vector<Register> isect;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(isect));
  return !isect.empty();
}
}  // namespace

Form* FormStack::pop_reg(Register reg,
                         const RegSet& barrier,
                         const Env& env,
                         bool allow_side_effects,
                         int begin_idx) {
  (void)env;  // keep this for easy debugging.
  RegSet modified;
  size_t begin = m_stack.size();
  if (begin_idx >= 0) {
    begin = begin_idx;
  }
  for (size_t i = begin; i-- > 0;) {
    auto& entry = m_stack.at(i);
    if (entry.active) {
      if (entry.destination.has_value() && entry.destination->reg() == reg) {
        entry.source->get_modified_regs(modified);
        if (!allow_side_effects && entry.source->has_side_effects()) {
          // the source of the set! has a side effect and that's not allowed, so abort.
          return nullptr;
        }
        if (nonempty_intersection(modified, barrier)) {
          // violating the barrier registers.
          return nullptr;
        }
        entry.active = false;
        assert(entry.source);
        if (entry.non_seq_source.has_value()) {
          assert(entry.sequence_point == false);
          auto result = pop_reg(entry.non_seq_source->reg(), barrier, env, allow_side_effects, i);
          if (result) {
            return result;
          }
        }
        return entry.source;
      } else {
        // we didn't match
        if (entry.sequence_point) {
          // and it's a sequence point! can't look any more back than this.
          return nullptr;
        }
        // no match, and not a sequence:
        if (entry.source) {
          assert(!entry.elt);
          entry.source->get_modified_regs(modified);
          if (!allow_side_effects) {
            // shouldn't allow skipping past a set! (may be too conservative?)
            return nullptr;
          }
        } else {
          assert(entry.elt);
          entry.elt->get_modified_regs(modified);
          if (!allow_side_effects && entry.elt->has_side_effects()) {
            // shouldn't allow skipping past something with a set! (also may be too conservative?)
            return nullptr;
          }
        }
      }
    } else {
      if (entry.destination.has_value() && entry.destination->reg() == reg) {
        return nullptr;
      }
    }
  }
  // we didn't have it...
  return nullptr;
}

Form* FormStack::unsafe_peek(Register reg, const Env& env) {
  RegSet modified;
  for (size_t i = m_stack.size(); i-- > 0;) {
    auto& entry = m_stack.at(i);
    if (entry.active) {
      fmt::print("PEEK ERROR {}:\n{}\n", reg.to_string(), print(env));
      throw std::runtime_error("Failed to unsafe peek 1");
    }

    entry.source->get_modified_regs(modified);
    if (modified.find(reg) != modified.end()) {
      throw std::runtime_error("Failed to unsafe peek 2");
    }

    if (entry.destination.has_value() && entry.destination->reg() == reg) {
      return entry.source;
    }
  }
  return nullptr;
}

FormElement* FormStack::pop_back(FormPool& pool) {
  auto& back = m_stack.back();
  assert(back.active);
  back.active = false;
  if (back.elt) {
    return back.elt;
  } else {
    assert(back.destination.has_value());
    auto elt = pool.alloc_element<SetVarElement>(*back.destination, back.source,
                                                 back.sequence_point, back.set_info);
    back.source->parent_element = elt;
    return elt;
  }
}

std::vector<FormElement*> FormStack::rewrite(FormPool& pool) {
  std::vector<FormElement*> result;

  for (auto& e : m_stack) {
    if (!e.active) {
      continue;
    }

    if (e.destination.has_value()) {
      auto elt =
          pool.alloc_element<SetVarElement>(*e.destination, e.source, e.sequence_point, e.set_info);
      e.source->parent_element = elt;
      result.push_back(elt);
    } else {
      result.push_back(e.elt);
    }
  }
  return result;
}

void rewrite_to_get_var(std::vector<FormElement*>& default_result,
                        FormPool& pool,
                        const Variable& var) {
  auto last_op_as_set = dynamic_cast<SetVarElement*>(default_result.back());
  if (last_op_as_set && last_op_as_set->dst().reg() == var.reg()) {
    default_result.pop_back();
    for (auto form : last_op_as_set->src()->elts()) {
      form->parent_form = nullptr;  // will get set later, this makes it obvious if I forget.
      default_result.push_back(form);
    }
  } else {
    default_result.push_back(pool.alloc_element<SimpleAtomElement>(SimpleAtom::make_var(var)));
  }
}

std::vector<FormElement*> rewrite_to_get_var(FormStack& stack,
                                             FormPool& pool,
                                             const Variable& var) {
  auto default_result = stack.rewrite(pool);
  rewrite_to_get_var(default_result, pool, var);
  return default_result;
}

}  // namespace decompiler
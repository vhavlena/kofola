// implementation of NCSB-based complementation algorithm for deterministic SCCs

#include "complement_alg_ncsb.hpp"

using namespace kofola;
using mstate_set = abstract_complement_alg::mstate_set;
using mstate_col_set = abstract_complement_alg::mstate_col_set;


namespace { // anonymous namespace

  /// returns true of there is at least one outgoing accepting transition from
  /// a set of states over the given symbol in the given component
  bool contains_accepting_outgoing_transitions(
    const spot::const_twa_graph_ptr&  aut,
    const spot::scc_info&             scc_inf,
    unsigned                          scc_num,
    const std::set<unsigned>&         states,
    const bdd&                        symbol)
  { // {{{
    for (unsigned s : states) {
      for (const auto &t : aut->out(s)) {
        if (scc_inf.scc_of(t.dst) == scc_num && bdd_implies(symbol, t.cond)) {
          if (t.acc) { return true; }
        }
      }
    }

    return false;
  } // contains_accepting_outgoing_transitions() }}}
}


complement_ncsb::mstate_ncsb::mstate_ncsb(
  const std::set<unsigned>&  check,
  const std::set<unsigned>&  safe,
  const std::set<unsigned>&  breakpoint,
  bool                       active
  ) :
  check_(check),
  safe_(safe),
  breakpoint_(breakpoint),
  active_(active)
{ }

std::string complement_ncsb::mstate_ncsb::to_string() const
{
  std::string res = std::string("[NCSB(") + ((this->active_)? "A" : "T") + "): ";
  res += "C=" + std::to_string(this->check_);
  res += ", S=" + std::to_string(this->safe_);
  if (this->active_) {
    res += ", B=" + std::to_string(this->breakpoint_);
  }
  res += "]";
  return res;
}

bool complement_ncsb::mstate_ncsb::eq(const mstate& rhs) const
{
  const mstate_ncsb* rhs_ncsb = dynamic_cast<const mstate_ncsb*>(&rhs);
  assert(rhs_ncsb);
  return (this->active_ == rhs_ncsb->active_) &&
    (this->check_ == rhs_ncsb->check_) &&
    (this->safe_ == rhs_ncsb->safe_) &&
    (this->breakpoint_ == rhs_ncsb->breakpoint_);
}

bool complement_ncsb::mstate_ncsb::lt(const mstate& rhs) const
{ // {{{
  const mstate_ncsb* rhs_ncsb = dynamic_cast<const mstate_ncsb*>(&rhs);
  assert(rhs_ncsb);

  if (this->active_ != rhs_ncsb->active_) { return this->active_ < rhs_ncsb->active_; }
  if (this->check_ != rhs_ncsb->check_) { return this->check_ < rhs_ncsb->check_; }
  if (this->safe_ != rhs_ncsb->safe_) { return this->safe_ < rhs_ncsb->safe_; }
  if (this->breakpoint_ != rhs_ncsb->breakpoint_) { return this->breakpoint_ < rhs_ncsb->breakpoint_; }

  return false;   // if all are equal
} // lt() }}}

complement_ncsb::mstate_ncsb::~mstate_ncsb()
{ }

complement_ncsb::complement_ncsb(const cmpl_info& info, unsigned scc_index)
  : abstract_complement_alg(info, scc_index)
{ }

mstate_set complement_ncsb::get_init() const
{ // {{{
  DEBUG_PRINT_LN("init NCSB for SCC " + std::to_string(this->scc_index_));
  std::set<unsigned> init_state;

  for (size_t i = 0; i < this->info_.aut_->num_states(); ++i) {
    DEBUG_PRINT_LN("state " + std::to_string(i) +"'s SCC: " +
      std::to_string(this->info_.scc_info_.scc_of(i)));
  }

  unsigned orig_init = this->info_.aut_->get_init_state_number();
  if (this->info_.scc_info_.scc_of(orig_init) == this->scc_index_) {
    init_state.insert(orig_init);
  }

  std::shared_ptr<mstate> ms(new mstate_ncsb(init_state, {}, {}, false));
  mstate_set result = {ms};
  return result;
} // get_init() }}}

mstate_col_set complement_ncsb::get_succ_track(
  const std::set<unsigned>&  glob_reached,
  const mstate*              src,
  const bdd&                 symbol) const
{
  const mstate_ncsb* src_ncsb = dynamic_cast<const mstate_ncsb*>(src);
  assert(src_ncsb);
  assert(!src_ncsb->active_);

  // check that safe states do not see accepting transition
  if (contains_accepting_outgoing_transitions(
      this->info_.aut_,
      this->info_.scc_info_,
      this->scc_index_,
      src_ncsb->safe_, symbol)) {
    return {};
  }

  std::set<unsigned> succ_safe = kofola::get_all_successors_in_scc(
    this->info_.aut_, this->info_.scc_info_, this->scc_index_, src_ncsb->safe_, symbol);

  std::set<unsigned> succ_states;
  for (unsigned st : glob_reached) {
    if (this->info_.scc_info_.scc_of(st) == this->scc_index_) {
      if (succ_safe.find(st) == succ_safe.end()) { // if not in safe
        succ_states.insert(st);
      }
    }
  }

  std::shared_ptr<mstate> ms(new mstate_ncsb(succ_states, succ_safe, {}, false));
  mstate_col_set result = {{ms, {}}}; return result;
} // get_succ_track() }}}

mstate_set complement_ncsb::lift_track_to_active(const mstate* src) const
{ // {{{
  const mstate_ncsb* src_ncsb = dynamic_cast<const mstate_ncsb*>(src);
  assert(src_ncsb);
  assert(!src_ncsb->active_);

  std::shared_ptr<mstate> ms(new mstate_ncsb(src_ncsb->check_, src_ncsb->safe_, src_ncsb->check_, true));
  return {ms};
} // lift_track_to_active() }}}

mstate_col_set complement_ncsb::get_succ_active(
  const std::set<unsigned>&  glob_reached,
  const mstate*              src,
  const bdd&                 symbol) const
{
  DEBUG_PRINT_LN("computing successor for glob_reached = " + std::to_string(glob_reached) +
    ", " + std::to_string(*src) + " over " + std::to_string(symbol));
  const mstate_ncsb* src_ncsb = dynamic_cast<const mstate_ncsb*>(src);
  assert(src_ncsb);
  assert(src_ncsb->active_);

  DEBUG_PRINT_LN("tracking successor of: " + std::to_string(*src_ncsb));
  mstate_ncsb tmp(src_ncsb->check_, src_ncsb->safe_, {}, false);
  mstate_col_set track_succ = this->get_succ_track(glob_reached, &tmp, symbol);

  if (track_succ.size() == 0) { return {};}
  assert(track_succ.size() == 1);

  const mstate_ncsb* track_ms = dynamic_cast<const mstate_ncsb*>(track_succ[0].first.get());
  assert(track_ms);

  DEBUG_PRINT_LN("obtained track ms: " + std::to_string(*track_ms));

  std::set<unsigned> tmp_break = kofola::get_all_successors_in_scc(
    this->info_.aut_, this->info_.scc_info_, this->scc_index_, src_ncsb->breakpoint_, symbol);

  DEBUG_PRINT_LN("tmp_break = " + std::to_string(tmp_break));

  std::set<unsigned> succ_break = get_set_difference(tmp_break, track_ms->safe_);
  if (succ_break.empty()) { // if we hit breakpoint
    std::shared_ptr<mstate> ms(new mstate_ncsb(track_ms->check_, track_ms->safe_, {}, false));
    return {{ms, {0}}};
  } else { // not breakpoint
    mstate_col_set result;
    std::shared_ptr<mstate> ms(new mstate_ncsb(track_ms->check_, track_ms->safe_, succ_break, true));
    DEBUG_PRINT_LN("standard successor: " + ms->to_string());
    result.push_back({ms, {}});

    // let us generate decreasing successor if the following three conditions hold:
    //   1) src_ncsb->breakpoint_ contains no accepting state
    //   2) succ_break contains no accepting state
    //   3) delta(src_ncsb->breakpoint_, symbol) contains no accepting condition

    // 1) check src_ncsb->breakpoint_ is not accepting
    if (kofola::set_contains_accepting_state(src_ncsb->breakpoint_,
      this->info_.state_accepting_)) {
      return result;
    }

    // 2) check succ_break contains no accepting state
    if (kofola::set_contains_accepting_state(succ_break,
      this->info_.state_accepting_)) {
      return result;
    }

    // 3) delta(src_ncsb->breakpoint_, symbol) contains no accepting condition
    if (contains_accepting_outgoing_transitions(
        this->info_.aut_,
        this->info_.scc_info_,
        this->scc_index_,
        src_ncsb->breakpoint_, symbol)) {
      return result;
    }

    // add the decreasing successor
    std::set<unsigned> decr_safe = get_set_union(track_ms->safe_, succ_break);
    std::set<unsigned> decr_check = get_set_difference(track_ms->check_, decr_safe);
    std::shared_ptr<mstate> decr_ms(new mstate_ncsb(decr_check, decr_safe, decr_check, true));
    DEBUG_PRINT_LN("decreasing successor: " + decr_ms->to_string());
    result.push_back({decr_ms, {0}});

    return result;
  }
}

complement_ncsb::~complement_ncsb()
{ }
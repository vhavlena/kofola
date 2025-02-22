// Copyright (C) 2017-2019 Laboratoire de Recherche et Développement
// de l'Epita.
// Copyright (C) 2020  The Seminator Authors
// Copyright (C) 2021  The COLA Authors
//
// COLA is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// COLA is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "optimizer.hpp"
//#include "struct.hpp"

#include <deque>
#include <map>

#include <spot/misc/hashfunc.hh>
#include <spot/twaalgos/isdet.hh>
#include <spot/twaalgos/sccinfo.hh>
#include <spot/twaalgos/isunamb.hh>

#include <spot/twaalgos/degen.hh>
#include <spot/twaalgos/simulation.hh>
#include <spot/twaalgos/determinize.hh>
#include <spot/twaalgos/parity.hh>

#include <spot/twaalgos/postproc.hh>

#include <spot/parseaut/public.hh>
#include <spot/twaalgos/hoa.hh>
#include <spot/misc/version.hh>
#include <spot/twa/acc.hh>

#include <types.hpp>

#define FWZ_DEBUG 0

// This file contains various complementation algorithms 

// This contains a modified version of Spot 2.8's complement_semidet()
// function (spot/twaalgos/complement.cc).  It will likely be
// contributed back to Spot in the future.
namespace from_spot
{
        class ncsb_complementation
                {
                private:
                    // The source automaton.
                    const spot::const_twa_graph_ptr aut_;

                    // SCCs information of the source automaton.
                    spot::scc_info si_;

                    // Number of states in the input automaton.
                    unsigned nb_states_;

                    // The complement being built.
                    spot::twa_graph_ptr res_;

                    // Association between NCSB states and state numbers of the
                    // complement.
                    std::unordered_map<small_mstate, unsigned, small_mstate_hash> ncsb2n_;

                    // States to process.
                    std::deque<std::pair<mstate, unsigned>> todo_;

                    // Support for each state of the source automaton.
                    std::vector<bdd> support_;

                    // Propositions compatible with all transitions of a state.
                    std::vector<bdd> compat_;

                    // Whether a SCC is deterministic or not
                    std::vector<bool> is_deter_;

                    // Whether a state only has accepting transitions
                    std::vector<bool> is_accepting_;

                    // State names for graphviz display
                    std::vector<std::string>* names_;

                    // Show NCSB states in state name to help debug
                    bool show_names_;

                    // opt 
                    bool optb_;

                    // on the fly
                    bool onthefly;

                    std::string
                    get_name(const small_mstate& ms)
                    {
                      std::string res = "{";

                      bool first_state = true;
                      for (const auto& p: ms)
                        if (p.second == ncsb_n)
                        {
                          if (!first_state)
                            res += ",";
                          first_state = false;
                          res += std::to_string(p.first);
                        }

                      res += "},{";

                      first_state = true;
                      for (const auto& p: ms)
                        if (p.second & ncsb_c)
                        {
                          if (!first_state)
                            res += ",";
                          first_state = false;
                          res += std::to_string(p.first);
                        }

                      res += "},{";

                      first_state = true;
                      for (const auto& p: ms)
                        if (p.second == ncsb_s)
                        {
                          if (!first_state)
                            res += ",";
                          first_state = false;
                          res += std::to_string(p.first);
                        }

                      res += "},{";

                      first_state = true;
                      for (const auto& p: ms)
                        if (p.second == ncsb_cb)
                        {
                          if (!first_state)
                            res += ",";
                          first_state = false;
                          res += std::to_string(p.first);
                        }

                      return res + "}";
                    }

                    small_mstate
                    to_small_mstate(const mstate& ms)
                    {
                      unsigned count = 0;
                      for (unsigned i = 0; i < nb_states_; ++i)
                        count+= (ms[i] != ncsb_m);
                      small_mstate small;
                      small.reserve(count);
                      for (unsigned i = 0; i < nb_states_; ++i)
                        if (ms[i] != ncsb_m)
                          small.emplace_back(i, ms[i]);
                      return small;
                    }

                    // From a NCSB state, looks for a duplicate in the map before
                    // creating a new state if needed.
                    unsigned
                    new_state(mstate&& s)
                    {
                      auto p = ncsb2n_.emplace(to_small_mstate(s), 0);
                      if (p.second) // This is a new state
                      {
                        p.first->second = res_->new_state();
                        if (show_names_)
                          names_->push_back(get_name(p.first->first));
                        todo_.emplace_back(std::move(s), p.first->second);
                      }
                      return p.first->second;
                    }

                    void
                    ncsb_successors(mstate&& ms, unsigned origin, bdd letter)
                    {

                      std::vector <mstate> succs;
                      succs.emplace_back(nb_states_, ncsb_m);

                      // PLDI: accepting transitions when B' would be empty
                      std::vector <bool> acc_succs;
                      acc_succs.push_back(false);

                      // Handle S states.
                      //
                      // Treated first because we can escape early if the letter
                      // leads to an accepting transition for a Safe state.
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        if (ms[i] != ncsb_s)
                          continue;

                        for (const auto &t: aut_->out(i))
                        {
                          if (!bdd_implies(letter, t.cond))
                            continue;
                          if (t.acc || is_accepting_[t.dst])
                            // Exit early; transition is forbidden for safe
                            // state.
                            return;

                          succs[0][t.dst] = ncsb_s;
                          // No need to look for other compatible transitions
                          // for this state; it's in the deterministic part of
                          // the automaton
                          break;
                        }
                      }
  
                      // record the set of states that come via accepting transitions
                      std::vector<bool> c_succs (nb_states_, false);
                      // Handle C states.
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        // including B-states
                        if (!(ms[i] & ncsb_c))
                          continue;

                        for (const auto &t: aut_->out(i))
                        {
                          if (!bdd_implies(letter, t.cond))
                            continue;

                          // PLDI optimization:
                          // Compute C' and remove states that are already in S'
                          // We have still only unique successor
                          if (succs[0][t.dst] == ncsb_m) 
                          {
                            succs[0][t.dst] = ncsb_c;
                           
                            if(optb_ && c_succs[t.dst] == false) {
                              c_succs[t.dst] = true;
                              
                            }
                          }
                          // No need to look for other compatible transitions
                          // for this state; it's in the deterministic part of
                          // the automaton
                          break;
                        }
                      }
                     
                      // Handle N states.
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        if (ms[i] != ncsb_n)
                          continue;
                        for (const auto &t: aut_->out(i))
                        {
                          if (!bdd_implies(letter, t.cond))
                            continue;

                          if (onthefly)
                          {
                            
                            if (t.acc)
                            {
                              if (succs[0][t.dst] == ncsb_m) 
                              {
                                succs[0][t.dst] = ncsb_c;
                              }
                            }      
                            else
                            {
                              for (auto &succ: succs) 
                              {
                                if (succ[t.dst] == ncsb_m) 
                                {
                                  succ[t.dst] = ncsb_n;
                                }
                              }
                            }               
                          } 
                          else 
                          {
                            // PLDI: All states from 2nd component go to C only.
                            // PLDI: We have still only a unique successor
                            if (is_deter_[si_.scc_of(t.dst)])
                            {
                              if (succs[0][t.dst] == ncsb_m) {
                                succs[0][t.dst] = ncsb_c;
                                }
                            } else
                              for (auto &succ: succs) {
                                succ[t.dst] = ncsb_n;
                              }
                          }
                        }
                      }
                     
                      // PLDI: Handle B states. We need to know what remained in C'.
                      // PLDI: We first move all successors to B', and then pereform
                      // branching in next pass
                      bool is_b_empty = true;
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        if (ms[i] != ncsb_cb)
                          continue;
                        is_b_empty = false;
                        bool has_succ = false;
                        for (const auto &t: aut_->out(i))
                        {
                          if (!bdd_implies(letter, t.cond))
                            continue;

                          has_succ = true;
                          if (succs[0][t.dst] == ncsb_c) {
                              succs[0][t.dst] = ncsb_cb;
                          }
                         
                          // PLDI: If t is not accepting and t.dst in S, stop
                          // because t.src should have been i S already.
                          if (!t.acc && (succs[0][t.dst] == ncsb_s))
                            return;

                          // No need to look for other compatible transitions
                          // for this state; it's in the deterministic part of
                          // the automaton
                          break;
                        }
                        if (!has_succ && !is_accepting_[i])
                          return;
                      }

                      // Allow to move accepting dst to S'
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        if (ms[i] != ncsb_cb)
                          continue;

                        for (const auto &t: aut_->out(i))
                        {
                          if (!bdd_implies(letter, t.cond))
                            continue;

                          if (t.acc)
                          {
                            // double all the current possible states
                            unsigned length = succs.size();
                            for (unsigned j = 0; j < length; ++j)
                            {
                              if ((succs[j][t.dst] == ncsb_cb) & (!is_accepting_[t.dst]))
                              {
                                succs.push_back(succs[j]);
                                succs.back()[t.dst] = ncsb_s;
                                acc_succs.push_back(false);
                              }
                            }
                          }
                        }
                      }

                      // PLDI: For each possible successor check if B' might be empty
                      // If yes, double the successors for each state in C', make edges
                      // to all clones accepting.
                      {
                        unsigned length = succs.size();
                        for (unsigned j = 0; j < length; ++j)
                        {
                          // Check for empty B'
                          bool b_empty = true;
                          for (unsigned i = 0; i < nb_states_; ++i)
                          {
                            if (succs[j][i] == ncsb_cb) {
                              b_empty = false;
                              break;
                            }
                          }

                          if (b_empty)
                          {
                            //PLDI: for each state s in C', move it to B'
                            // if s is not accepting make a clone
                            // of all succs in new_succs where s is in S'
                            for (unsigned i = 0; i < nb_states_; ++i) {
                              // without lazyOpt
                            
                              if(optb_ ) {
                                // only copy states in C' to B'
                                // note here that the states in B' after branching may not occur in C'
                                // so we need to only concern states that remain in C'
                                if(! (c_succs[i] && succs[j][i] == ncsb_c)) {
                                  continue;
                                }
                              }else {
                                if (succs[j][i] != ncsb_c) {
                                  continue;
                                }
                              }
                              
                              succs[j][i] = ncsb_cb;
                              
                            }
                            
                            // Set edge as accepting
                            acc_succs[j] = true;
                            std::vector <mstate> new_succs; // Store clones of current succ
                            new_succs.push_back(succs[j]);
                           
                            //PLDI: for each state s in C'
                            // if s is not accepting make a clone
                            // of all succs in new_succs where s is in S'
                            for (unsigned i = 0; i < nb_states_; ++i) {
                              // these are all states in C', also in B'
                              if(optb_) {
                                // only branching C states
                                if(succs[j][i] != ncsb_c && succs[j][i] != ncsb_cb) {
                                  continue;
                                }
                              }else {
                                if(succs[j][i] != ncsb_cb) {
                                  continue;
                                }
                              }
                              
                              {
                                unsigned k_length = new_succs.size();
                                for (unsigned k = 0; k < k_length; ++k) {
                                  //PLDI: skip accepting states
                                  if (is_accepting_[i])
                                    continue;
                                  // Make copy of k with i moved from C to S
                                  new_succs.push_back(new_succs[k]);
                                  new_succs.back()[i] = ncsb_s;
                                }
                              }
                              // new_succs[0] is succ[j] with C -> CB
                              succs[j] = new_succs[0];
                              // Move the rest to the end of succ
                              unsigned k_length = new_succs.size();
                              for (unsigned k = 1; k < k_length; ++k) {
                                succs.push_back(new_succs[k]);
                                acc_succs.push_back(true);
                              }
                            }
                          }

                        }
                      }

                      // Create the automaton states
                      unsigned length = succs.size();
                      for (unsigned j = 0; j < length; ++j)
                      {
                        if (acc_succs[j])
                        {
                          unsigned dst = new_state(std::move(succs[j]));
                
                          res_->new_edge(origin, dst, letter, {0});
                        } else {
                          unsigned dst = new_state(std::move(succs[j]));
                          res_->new_edge(origin, dst, letter);
                        }
                      }
                    }

                public:
                    ncsb_complementation(const spot::const_twa_graph_ptr& aut, bool show_names)
                            : aut_(aut),
                              si_(aut),
                              nb_states_(aut->num_states()),
                              support_(nb_states_),
                              compat_(nb_states_),
                              is_accepting_(nb_states_),
                              show_names_(show_names)
                    {
                      res_ = spot::make_twa_graph(aut->get_dict());
                      res_->copy_ap_of(aut);
                      res_->set_buchi();

                      // Generate bdd supports and compatible options for each state.
                      // Also check if all its transitions are accepting.
                      for (unsigned i = 0; i < nb_states_; ++i)
                      {
                        bdd res_support = bddtrue;
                        bdd res_compat = bddfalse;
                        bool accepting = true;
                        bool has_transitions = false;
                        for (const auto& out: aut->out(i))
                        {
                          has_transitions = true;
                          res_support &= bdd_support(out.cond);
                          res_compat |= out.cond;
                          if (!out.acc)
                            accepting = false;
                        }
                        support_[i] = res_support;
                        compat_[i] = res_compat;
                        is_accepting_[i] = accepting && has_transitions;
                      }

                      optb_ = false;
                      onthefly = false;

                      if (show_names_)
                      {
                        names_ = new std::vector<std::string>();
                        res_->set_named_prop("state-names", names_);
                      }

                      // Because we only handle one initial state, we assume it
                      // belongs to the N set. (otherwise the automaton would be
                      // deterministic)
                      unsigned init_state = aut->get_init_state_number();
                      mstate new_init_state(nb_states_, ncsb_m);
                      new_init_state[init_state] = ncsb_n;
                      res_->set_init_state(new_state(std::move(new_init_state)));
                    }

                    void set_opt() 
                    {
                      optb_ = true;
                    }

                    void set_onthefly()
                    {
                      onthefly = true;
                    }
                    
                    spot::twa_graph_ptr
                    run()
                    {
                      // Main stuff happens here


                      if (onthefly == false)
                      {
                        // Compute which SCCs are part of the deterministic set.
                        is_deter_ = spot::semidet_sccs(si_);
                      }
                    

                      while (!todo_.empty())
                      {
                        auto top = todo_.front();
                        todo_.pop_front();

                        mstate ms = top.first;

                        // Compute support of all available states.
                        bdd msupport = bddtrue;
                        bdd n_s_compat = bddfalse;
                        bdd c_compat = bddtrue;
                        bool c_empty = true;
                        for (unsigned i = 0; i < nb_states_; ++i)
                          if (ms[i] != ncsb_m)
                          {
                            msupport &= support_[i];
                            // PLDI: add ms[i] == ncsb_c as those states could be also virtually in S
                            if (ms[i] == ncsb_n || ms[i] == ncsb_s || ms[i] == ncsb_c || is_accepting_[i])
                              n_s_compat |= compat_[i];
                            else
                            {
                              c_empty = false;
                              c_compat &= compat_[i];
                            }
                          }

                        bdd all;
                        if (!c_empty)
                          all = c_compat;
                        else
                        {
                          all = n_s_compat;
                          if (all != bddtrue)
                          {
                            mstate empty_state(nb_states_, ncsb_m);
                            res_->new_edge(top.second,
                                          new_state(std::move(empty_state)),
                                          !all,
                                          {0});
                          }
                        }
                        while (all != bddfalse)
                        {
                          bdd one = bdd_satoneset(all, msupport, bddfalse);
                          all -= one;

                          // Compute all new states available from the generated
                          // letter.
                          ncsb_successors(std::move(ms), top.second, one);
                        }
                      }

                      res_->merge_edges();
                      return res_;
                    }
                };

    spot::twa_graph_ptr
    complement_semidet(const spot::const_twa_graph_ptr& aut, bool show_names)
    {
      if (!is_semi_deterministic(aut))
        throw std::runtime_error
                ("complement_semidet() requires a semi-deterministic input");
      auto ncsb = ncsb_complementation(aut, show_names);
      return ncsb.run();
    }

}

namespace cola 
{

     // fengwz
        class ncb_complementation
        {
        private:
            // The source automaton.
            const spot::const_twa_graph_ptr aut_;

            // SCCs information of the source automaton.
            spot::scc_info si_;

            // Number of states in the input automaton.
            unsigned nb_states_;

            // The complement being built.
            spot::twa_graph_ptr res_;

            // Association between NCB states and state numbers of the
            // complement.
            std::unordered_map<small_macrostate, unsigned, small_macrostate_hash> ncb2n_;
            // std::unordered_map<macrostate, unsigned, small_macrostate_hash> ncb2n_;

            // States to process.
            std::deque<std::pair<macrostate, unsigned>> todo_;

            // Support for each state of the source automaton.
            std::vector<bdd> support_;

            // Propositions compatible with all transitions of a state.
            std::vector<bdd> compat_;

            // Whether a SCC is deterministic or not
            std::vector<bool> is_deter_;

            // Whether a state only has accepting transitions
            std::vector<bool> is_accepting_;

            // State names for graphviz display
            std::vector<std::string>* names_;

            // Show NCB states in state name to help debug
            bool show_names_;
            
            std::string
            get_name(const small_macrostate& ms)
            {
              std::string res = "{";

              bool first_state = true;
              for (const auto& p: ms)
                if (p.second & ncb_n || p.second == ncb_i)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  if (p.second == ncb_i)
                    res += ("i" + std::to_string(p.first));
                  else
                    res += std::to_string(p.first);
                }

              res += "},{";

              first_state = true;
              for (const auto& p: ms)
                if (p.second == ncb_c || p.second == ncb_b)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              res += "},{";

              first_state = true;
              for (const auto& p: ms)
                if (p.second == ncb_b)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              return res + "}";
            }

            // delete unreachable states (ncb = ncb_m)
            small_macrostate
            to_small_macrostate(const macrostate& ms)
            {
              unsigned count = 0;
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                count+= (ms[i] != ncb_m);
              }
              small_macrostate small;
              small.reserve(count);
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != ncb_m)
                  small.emplace_back(i, ms[i]);
              }
              return small;
            }

            // input a NCB state
            // return unsigned 
            // looks for a duplicate in the map before creating a new state if needed.
            unsigned
            new_state(macrostate&& s)
            {
              // <small_macrostate, unsigned>
              auto p = ncb2n_.emplace(to_small_macrostate(s), 0);
              if (p.second) // This is a new state
              {
                p.first->second = res_->new_state();
                if (show_names_)
                  names_->push_back(get_name(p.first->first));
                todo_.emplace_back(std::move(s), p.first->second);
              }
              return p.first->second;
            }

            // accepting phase: (n,c,b) to (n,c,b)
            void
            acc_successors(macrostate&& ms, unsigned origin, bdd letter)
            {
              std::vector <macrostate> succs;
              succs.emplace_back(nb_states_, ncb_m);

              std::unordered_map <unsigned, unsigned> dstSrc;

              // Handle N states first beacause C' needs N'
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (!(ms[i] & ncb_n))
                  continue;

                for (const auto& t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;
                  succs[0][t.dst] = ncb_n;
                  // N' \cap F \in C'
                  if (t.acc)
                    succs[0][t.dst] = ncb_c;
                  dstSrc.emplace(t.dst, i);
                }
              }

              // Handle C states: (C,a) \cup (N' \cap F)
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if ((ms[i] != ncb_c) && (ms[i] != ncb_b))
                  continue;
  
                for (auto& t: dstSrc)
                {
                  if (t.second == i)
                  {
                    succs[0][t.first] = ncb_c;
                  }
                }
              }

              // Handle B states
              bool b_empty = true;
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != ncb_b)
                  continue;
                
                // B set is not empty
                b_empty = false;

                for (auto& t: dstSrc)
                {
                  if (t.second == i)
                  {
                    succs[0][t.first] = ncb_b;
                  }
                }
              }

              // if B is empty
              if (b_empty)
              {
                for (auto& succ: succs)
                  for (unsigned i = 0; i < succ.size(); ++i)
                    if (succ[i] == ncb_c)
                      succ[i] = ncb_b;
              }


              // Create the automaton states
              for (auto& succ: succs)
              {
                bool b_empty = true;
                for (const auto& state: succ)
                  if (state == ncb_b)
                  {
                    b_empty = false;
                    break;
                  }

                // accepting state
                // new edge: origin to dst
                // if b set in dst is empty, label this edge as an accepting edge
                if (b_empty)
                {
                  unsigned dst = new_state(std::move(succ));
                  res_->new_edge(origin, dst, letter, {0});
                }
                else
                {
                  unsigned dst = new_state(std::move(succ));
                  res_->new_edge(origin, dst, letter);
                }
              }

              
            }

            // initial phase to initial phase  
            // initial phase to accepting phase
            void 
            init_successors(macrostate&& ms, unsigned origin, bdd letter)
            {
              std::vector<macrostate> succs;
              succs.emplace_back(nb_states_, ncb_m);

              // subset to subset
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                // some states == ncb_m, missing them.
                if (ms[i] != ncb_i)
                  continue;
                
                for (const auto& t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;
                  succs[0][t.dst] = ncb_i;
                }
              }

              for (auto& succ: succs)
              {
                unsigned dst = new_state(std::move(succ));
                res_->new_edge(origin, dst, letter);
              }

              // subset to (N, C, B)
              // succs.push_back(nb_states_, ncb_m);
              // std::vector<macrostate> tmpStates;
              // tmpStates.emplace_back(nb_states_, ncb_m);
              // std::vector<bool> visit(nb_states_, false);
              macrostate tmpState(nb_states_, ncb_m);

              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != ncb_i)
                  continue;
                tmpState[i] = ncb_n;
              }

              acc_successors(std::move(tmpState), origin, letter);
            }

            // combine 
            void
            ncb_successors(macrostate&& ms, unsigned origin, bdd letter)
            {
              int flag = 1;
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                // initial phase
                if (ms[i] == ncb_i)
                {
                  flag = 1;
                  break;
                }

                // NCB
                if ((ms[i] & ncb_n))
                {
                  flag = 0;
                  break;
                }
              }

              if (flag == 1)
              {
                init_successors(std::move(ms), origin, letter);
              }
              else
              {
                acc_successors(std::move(ms), origin, letter);
              }
            }
           

        public:
            ncb_complementation(const spot::const_twa_graph_ptr& aut, bool show_names)
                    : aut_(aut),
                      si_(aut),
                      nb_states_(aut->num_states()),
                      support_(nb_states_),
                      compat_(nb_states_),
                      is_accepting_(nb_states_),
                      show_names_(show_names)
            {
              res_ = spot::make_twa_graph(aut->get_dict());
              res_->copy_ap_of(aut);
              res_->set_buchi();

              // Generate bdd supports and compatible options for each state.
              // Also check if all its transitions are accepting.
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                bdd res_support = bddtrue;
                bdd res_compat = bddfalse;
                bool accepting = true;
                bool has_transitions = false;
                for (const auto& out: aut->out(i))
                {
                  has_transitions = true;
                  res_support &= bdd_support(out.cond);
                  res_compat |= out.cond;
                  if (!out.acc)
                    accepting = false;
                }
                support_[i] = res_support;
                compat_[i] = res_compat;
                is_accepting_[i] = accepting && has_transitions;
              }



              // // Compute which SCCs are part of the deterministic set.
              // is_deter_ = spot::semidet_sccs(si_);

              if (show_names_)
              {
                names_ = new std::vector<std::string>();
                res_->set_named_prop("state-names", names_);
              }

              // Because we only handle one initial state, we assume it
              // belongs to the N set. (otherwise the automaton would be
              // deterministic)
              unsigned init_state = aut->get_init_state_number();

              // macrostate is a vector, whose length is nb_states_ and every element is ncb_m.
              macrostate new_init_state(nb_states_, ncb_m);
              new_init_state[init_state] = ncb_i;
              res_->set_init_state(new_state(std::move(new_init_state)));
            }

            spot::twa_graph_ptr
            run()
            {
              // Main stuff happens here

              while (!todo_.empty())
              {
                auto top = todo_.front();
                todo_.pop_front();

                macrostate ms = top.first;

                // Compute support of all available states.
                bdd msupport = bddtrue;
                bdd n_s_compat = bddfalse;
                bdd c_compat = bddtrue;
                bool c_empty = true;
                for (unsigned i = 0; i < nb_states_; ++i)
                  if (ms[i] != ncb_m)
                  {
                    msupport &= support_[i];                
                    n_s_compat |= compat_[i];
                  }

                bdd all;
                all = n_s_compat;

                // Get a mstate from todo_
                // 1. mstate -> (letter that not appears) empty set
                // 2. mstate -> (appearing letter) other mstate 
                if (all != bddtrue)
                {
                  macrostate empty_state(nb_states_, ncb_m);
                  res_->new_edge(top.second,
                                  new_state(std::move(empty_state)),
                                  !all,
                                  {0});
                }
                
                while (all != bddfalse)
                {
                  bdd one = bdd_satoneset(all, msupport, bddfalse);
                  all -= one;

                  // Compute all new states available from the generated letter.
                  ncb_successors(std::move(ms), top.second, one);
                }
              }

              res_->merge_edges();
              return res_;
            }
        };

        class nsbc_complementation
        {
        private:
            // The source automaton.
            const spot::const_twa_graph_ptr aut_;

            // SCCs information of the source automaton.
            spot::scc_info si_;

            // Number of states in the input automaton.
            unsigned nb_states_;

            // The complement being built.
            spot::twa_graph_ptr res_;

            // Association between NCSB states and state numbers of the
            // complement.
            std::unordered_map<small_mcstate, unsigned, small_mcstate_hash> nsbc2n_;

            // States to process.
            std::deque<std::pair<mcstate, unsigned>> todo_;

            // Support for each state of the source automaton.
            std::vector<bdd> support_;

            // Propositions compatible with all transitions of a state.
            std::vector<bdd> compat_;

            // Whether a SCC is deterministic or not
            std::vector<bool> is_deter_;

            // Whether a state only has accepting transitions
            std::vector<bool> is_accepting_;

            // State names for graphviz display
            std::vector<std::string>* names_;

            // Show NCSB states in state name to help debug
            bool show_names_;

            std::string
            get_name(const small_mcstate& ms)
            {
              std::string res = "{";

              // init phase
              bool first_state = true;
              for (const auto& p: ms)
                if (p.second == nsbc_i)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += ("i" + std::to_string(p.first));
                }

              res += "},{";

              // N set
              first_state = true;
              for (const auto& p: ms)
                if (p.second == nsbc_n)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              res += "},{";
              
              // S set
              first_state = true;
              for (const auto& p: ms)
                if (p.second == nsbc_s)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              res += "},{";

              // B set
              first_state = true;
              for (const auto& p: ms)
                if (p.second == nsbc_b)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              res += "},{";

              // C set
              first_state = true;
              for (const auto& p: ms)
                if (p.second == nsbc_c)
                {
                  if (!first_state)
                    res += ",";
                  first_state = false;
                  res += std::to_string(p.first);
                }

              return res + "}";
            }

            small_mcstate
            to_small_mcstate(const mcstate& ms)
            {
              unsigned count = 0;
              for (unsigned i = 0; i < nb_states_; ++i)
                count+= (ms[i] != nsbc_m);
              small_mcstate small;
              small.reserve(count);
              for (unsigned i = 0; i < nb_states_; ++i)
                if (ms[i] != nsbc_m)
                  small.emplace_back(i, ms[i]);
              return small;
            }

            // looks for a duplicate in the map before
            // creating a new state if needed.
            unsigned
            new_state(mcstate&& s)
            {
              auto p = nsbc2n_.emplace(to_small_mcstate(s), 0);
              if (p.second) // This is a new state
              {
                p.first->second = res_->new_state();
                if (show_names_)
                  names_->push_back(get_name(p.first->first));
                todo_.emplace_back(std::move(s), p.first->second);
              }
              return p.first->second;
            }

            void
            acc_successors(mcstate&& ms, unsigned origin, bdd letter)
            {
              // Here if we just define a mcstate succ is also ok 
              // It remains to be optimized
              mcstate succ(nb_states_, nsbc_m);

              // Handle S states. 
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != nsbc_s)
                  continue;

                for (const auto &t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;

                  if (t.acc)
                  {
                    if (succ[t.dst] != nsbc_s)
                      succ[t.dst] = nsbc_c;
                  }
                  else
                  {
                    succ[t.dst] = nsbc_s;
                  }
                }     
              }
               
              // Handle B states
              bool b_empty = true;
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != nsbc_b)
                  continue;
                
                // B set is not empty
                b_empty = false;

                for (const auto& t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;

                  if (succ[t.dst] != nsbc_s)
                    succ[t.dst] = nsbc_b;    
                  break;
                }
              }

              // Handle N states.
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != nsbc_n)
                  continue;
                for (const auto &t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;

                  // t.dst is in Q2
                  if (is_deter_[si_.scc_of(t.dst)])
                  {
                    if ((succ[t.dst] != nsbc_s) && (succ[t.dst] != nsbc_b))
                      succ[t.dst] = nsbc_c;
                  } 
                  else
                  {
                    succ[t.dst] = nsbc_n;
                  }
                }
              }

              // Handle C states.
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != nsbc_c)
                  continue;

                for (const auto &t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;

                  // remove S' and B' (if a state is labeled as S' or B', we skip it)
                  if ((succ[t.dst] != nsbc_s) && (succ[t.dst] != nsbc_b))
                    succ[t.dst] = nsbc_c;      
                 
                  break;
                }
              }

              // if B set is empty, move C' to B'
              if (b_empty)
              {
                for (unsigned i = 0; i < nb_states_; ++i)
                {
                  if(succ[i] == nsbc_c)
                    succ[i] = nsbc_b;
                }
              }
      
              // Create the automaton states
              bool b_succ_empty = true;
              for (const auto& state: succ)
              {
                if (state == nsbc_b)
                {
                  b_succ_empty = false;
                  break;
                }
              }

              // accepting state
              // new edge: origin to dst
              // if b set in dst is empty, label this edge as an accepting edge
              if (b_succ_empty)
              {
                unsigned dst = new_state(std::move(succ));
                res_->new_edge(origin, dst, letter, {0});
              }
              else
              {
                unsigned dst = new_state(std::move(succ));
                res_->new_edge(origin, dst, letter);
              }
            }

            void 
            init_successors(mcstate&& ms, unsigned origin, bdd letter)
            {
              mcstate succ(nb_states_, nsbc_m);

              // subset to subset
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                // some states == ncb_m, missing them.
                if (ms[i] != nsbc_i)
                  continue;
                
                for (const auto& t: aut_->out(i))
                {
                  if (!bdd_implies(letter, t.cond))
                    continue;
                  succ[t.dst] = nsbc_i;
                }
              }

              
              unsigned dst = new_state(std::move(succ));
              res_->new_edge(origin, dst, letter);
          

              // subset to (N, S, B, C)
              // succs.push_back(nb_states_, ncb_m);
              mcstate tmpState(nb_states_, nsbc_m);

              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] != nsbc_i)
                  continue;

                // i is in Q2 
                if (is_deter_[si_.scc_of(i)])
                {
                  for (const auto& t : aut_->out(i))
                  {
                    // i->(t) t.dst
                    // if the outgoing edge t is accepting
                    // put i into B set
                    if (!bdd_implies(letter, t.cond))
                      continue;

                    if (t.acc) 
                      tmpState[i] = nsbc_b;
                    else
                      tmpState[i] = nsbc_s;
                  }
                }
                else // i is in Q1
                  tmpState[i] = nsbc_n;
              }

              acc_successors(std::move(tmpState), origin, letter);
            }

            void
            nsbc_successors(mcstate&& ms, unsigned origin, bdd letter)
            {

              int flag = 1;
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                if (ms[i] == nsbc_m)
                  continue;

                if (ms[i] == nsbc_i)
                  break;
                else
                  flag = 0;
              }

              if (flag == 1)
              {
                init_successors(std::move(ms), origin, letter);
              }
              else
              {
                acc_successors(std::move(ms), origin, letter);
              }
            }

        public:
            nsbc_complementation(const spot::const_twa_graph_ptr& aut, bool show_names)
                    : aut_(aut),
                      si_(aut),
                      nb_states_(aut->num_states()),
                      support_(nb_states_),
                      compat_(nb_states_),
                      is_accepting_(nb_states_),
                      show_names_(show_names)
            {
              res_ = spot::make_twa_graph(aut->get_dict());
              res_->copy_ap_of(aut);
              res_->set_buchi();

              // Generate bdd supports and compatible options for each state.
              // Also check if all its transitions are accepting.
              for (unsigned i = 0; i < nb_states_; ++i)
              {
                bdd res_support = bddtrue;
                bdd res_compat = bddfalse;
                
                bool accepting = true;
                bool has_transitions = false;
                for (const auto& out: aut->out(i))
                {
                  has_transitions = true;
                  res_support &= bdd_support(out.cond);
                  res_compat |= out.cond;
                  if (!out.acc)
                    accepting = false;
                }
                support_[i] = res_support;
                compat_[i] = res_compat;
                is_accepting_[i] = accepting && has_transitions;
              }



              // Compute which SCCs are part of the deterministic set.
              is_deter_ = spot::semidet_sccs(si_);

              if (show_names_)
              {
                names_ = new std::vector<std::string>();
                res_->set_named_prop("state-names", names_);
              }

              // Because we only handle one initial state, we assume it
              // belongs to the N set. (otherwise the automaton would be
              // deterministic)
              unsigned init_state = aut->get_init_state_number();
              mcstate new_init_state(nb_states_, nsbc_m);
              new_init_state[init_state] = nsbc_i;
              res_->set_init_state(new_state(std::move(new_init_state)));
            }

            spot::twa_graph_ptr
            run()
            {
              // Main stuff happens here

              while (!todo_.empty())
              {
                auto top = todo_.front();
                todo_.pop_front();

                mcstate ms = top.first;

                // Compute support of all available states.
                bdd msupport = bddtrue;
                bdd n_s_compat = bddfalse;
                bdd c_compat = bddtrue;
                bool c_empty = true;
                for (unsigned i = 0; i < nb_states_; ++i)
                  if (ms[i] != nsbc_m)
                  {
                    msupport &= support_[i];                
                    n_s_compat |= compat_[i];
                  }

                bdd all;
                all = n_s_compat;

                // Get a mstate from todo_
                // 1. mstate -> (letter that not appears) empty set
                // 2. mstate -> (appearing letter) other mstate 
                if (all != bddtrue)
                {
                  mcstate empty_state(nb_states_, nsbc_m);
                  res_->new_edge(top.second,
                                  new_state(std::move(empty_state)),
                                  !all,
                                  {0});
                }
                
                while (all != bddfalse)
                {
                  bdd one = bdd_satoneset(all, msupport, bddfalse);
                  all -= one;

                  // Compute all new states available from the generated letter.
                  nsbc_successors(std::move(ms), top.second, one);
                }
              }

              res_->merge_edges();
              return res_;
            }
        };

    
       


    spot::twa_graph_ptr
    complement_semidet_onthefly(const spot::const_twa_graph_ptr& aut, bool show_names)
    {
      if (!is_semi_deterministic(aut))
        throw std::runtime_error
                ("complement_semidet() requires a semi-deterministic input");
      auto ncsb = from_spot::ncsb_complementation(aut, show_names);
      ncsb.set_onthefly();
      return ncsb.run();
    }

    spot::twa_graph_ptr
    complement_semidet_opt(const spot::const_twa_graph_ptr& aut, bool show_names)
    {
      if (!is_semi_deterministic(aut))
        throw std::runtime_error
                ("complement_semidet() requires a semi-deterministic input");
      auto ncsb = from_spot::ncsb_complementation(aut, show_names);
      ncsb.set_opt();
      return ncsb.run();
    }

    spot::twa_graph_ptr
    complement_semidet_opt_onthefly(const spot::const_twa_graph_ptr& aut, bool show_names)
    {
      if (!is_semi_deterministic(aut))
        throw std::runtime_error
                ("complement_semidet() requires a semi-deterministic input");
      auto ncsb = from_spot::ncsb_complementation(aut, show_names);
      ncsb.set_opt();
      ncsb.set_onthefly();
      return ncsb.run();
    }


    // fengwz
    spot::twa_graph_ptr
    complement_unambiguous(const spot::const_twa_graph_ptr &aut, bool show_names)
    {
      if (!is_unambiguous(aut))
        throw std::runtime_error
                ("complement_unambiguous() requires an unambiguous input");

      auto ncb = ncb_complementation(aut, show_names);
      return ncb.run();
    }

    // new complement_semidet
    spot::twa_graph_ptr
    new_complement_semidet(const spot::const_twa_graph_ptr& aut, bool show_names)
    {
      if (!is_semi_deterministic(aut))
        throw std::runtime_error
                ("complement_semidet() requires a semi-deterministic input");

      auto nsbc = nsbc_complementation(aut, show_names);
      return nsbc.run();
    }
}




/**
 * Copyright (c) 2022, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <utility>
#include <vector>

#include "document.sections.hh"

#include "base/enum_util.hh"
#include "base/itertools.hh"
#include "base/lnav_log.hh"
#include "base/opt_util.hh"
#include "data_scanner.hh"

namespace lnav {
namespace document {

nonstd::optional<hier_node*>
hier_node::lookup_child(section_key_t key) const
{
    return make_optional_from_nullable(key.match(
        [this](const std::string& str) -> hier_node* {
            auto iter = this->hn_named_children.find(str);
            if (iter != this->hn_named_children.end()) {
                return iter->second;
            }
            return nullptr;
        },
        [this](size_t index) -> hier_node* {
            if (index < this->hn_children.size()) {
                return this->hn_children[index].get();
            }
            return nullptr;
        }));
}

nonstd::optional<const hier_node*>
hier_node::lookup_path(const hier_node* root,
                       const std::vector<section_key_t>& path)
{
    auto retval = make_optional_from_nullable(root);

    for (const auto& comp : path) {
        if (!retval) {
            break;
        }

        retval = retval.value()->lookup_child(comp);
    }

    if (!retval) {
        return nonstd::nullopt;
    }

    return retval;
}

metadata
discover_metadata(const attr_line_t& al)
{
    const auto& orig_attrs = al.get_attrs();
    auto headers = orig_attrs
        | lnav::itertools::filter_in([](const string_attr& attr) {
                       if (attr.sa_type != &VC_ROLE) {
                           return false;
                       }

                       auto role = attr.sa_value.get<role_t>();
                       switch (role) {
                           case role_t::VCR_H1:
                           case role_t::VCR_H2:
                           case role_t::VCR_H3:
                           case role_t::VCR_H4:
                           case role_t::VCR_H5:
                           case role_t::VCR_H6:
                               return true;
                           default:
                               return false;
                       }
                   })
        | lnav::itertools::sort_by(&string_attr::sa_range);

    // Remove headers from quoted text
    for (const auto& orig_attr : orig_attrs) {
        if (orig_attr.sa_type == &VC_ROLE
            && orig_attr.sa_value.get<role_t>() == role_t::VCR_QUOTED_TEXT)
        {
            remove_string_attr(headers, orig_attr.sa_range);
        }
    }

    std::vector<section_interval_t> intervals;

    struct open_interval_t {
        open_interval_t(uint32_t level, file_off_t start, section_key_t id)
            : oi_level(level), oi_start(start), oi_id(std::move(id))
        {
        }

        uint32_t oi_level;
        file_off_t oi_start;
        section_key_t oi_id;
        std::unique_ptr<hier_node> oi_node{std::make_unique<hier_node>()};
    };
    std::vector<open_interval_t> open_intervals;
    auto root_node = std::make_unique<hier_node>();

    for (const auto& hdr_attr : headers) {
        auto role = hdr_attr.sa_value.get<role_t>();
        auto role_num = lnav::enums::to_underlying(role)
            - lnav::enums::to_underlying(role_t::VCR_H1);
        std::vector<open_interval_t> new_open_intervals;

        for (auto& oi : open_intervals) {
            if (oi.oi_level >= role_num) {
                // close out this section
                intervals.emplace_back(
                    oi.oi_start, hdr_attr.sa_range.lr_start - 1, oi.oi_id);
                auto* node_ptr = oi.oi_node.get();
                auto* parent_node = oi.oi_node->hn_parent;
                if (parent_node != nullptr) {
                    parent_node->hn_children.emplace_back(
                        std::move(oi.oi_node));
                    parent_node->hn_named_children.insert({
                        oi.oi_id.get<std::string>(),
                        node_ptr,
                    });
                }
            } else {
                new_open_intervals.emplace_back(std::move(oi));
            }
        }
        auto* parent_node = new_open_intervals.empty()
            ? root_node.get()
            : new_open_intervals.back().oi_node.get();
        new_open_intervals.emplace_back(role_num,
                                        hdr_attr.sa_range.lr_start,
                                        al.get_substring(hdr_attr.sa_range));
        new_open_intervals.back().oi_node->hn_parent = parent_node;
        new_open_intervals.back().oi_node->hn_start
            = hdr_attr.sa_range.lr_start;

        open_intervals = std::move(new_open_intervals);
    }

    for (auto& oi : open_intervals) {
        // close out this section
        intervals.emplace_back(oi.oi_start, al.length(), oi.oi_id);
        auto* node_ptr = oi.oi_node.get();
        auto* parent_node = oi.oi_node->hn_parent;
        if (parent_node == nullptr) {
            root_node = std::move(oi.oi_node);
        } else {
            parent_node->hn_children.emplace_back(std::move(oi.oi_node));
            parent_node->hn_named_children.insert({
                oi.oi_id.get<std::string>(),
                node_ptr,
            });
        }
    }

    return {
        sections_tree_t{std::move(intervals)},
        std::move(root_node),
    };
}

class structure_walker {
public:
    explicit structure_walker(string_fragment content) : sw_scanner(content)
    {
        this->sw_interval_state.resize(1);
        this->sw_hier_nodes.push_back(std::make_unique<hier_node>());
    }

    metadata walk()
    {
        pcre_context_static<30> pc;
        data_token_t dt = DT_INVALID;
        lnav::document::metadata retval;
        auto& pi = this->sw_scanner.get_input();

        while (this->sw_scanner.tokenize2(pc, dt)) {
            element el(dt, pc);

            switch (dt) {
                case DT_XML_DECL_TAG:
                case DT_XML_EMPTY_TAG:
                    this->sw_values.emplace_back(el);
                    break;
                case DT_XML_OPEN_TAG:
                    this->flush_values();
                    this->sw_interval_state.back().is_start
                        = el.e_capture.c_begin;
                    this->sw_interval_state.back().is_line_number
                        = this->sw_line_number;
                    this->sw_interval_state.back().is_name
                        = pi.get_substr(&el.e_capture);
                    this->sw_depth += 1;
                    this->sw_interval_state.resize(this->sw_depth + 1);
                    this->sw_hier_nodes.push_back(
                        std::make_unique<hier_node>());
                    break;
                case DT_XML_CLOSE_TAG: {
                    auto term = this->flush_values();
                    if (this->sw_depth > 0) {
                        this->sw_depth -= 1;
                        this->append_child_node(term);
                        this->sw_interval_state.pop_back();
                        this->sw_hier_stage
                            = std::move(this->sw_hier_nodes.back());
                        this->sw_hier_nodes.pop_back();
                    }
                    this->append_child_node(el.e_capture);
                    this->flush_values();
                    break;
                }
                case DT_LCURLY:
                case DT_LSQUARE:
                case DT_LPAREN:
                    this->flush_values();
                    this->sw_depth += 1;
                    if (!this->sw_interval_state.back().is_start) {
                        this->sw_interval_state.back().is_start
                            = el.e_capture.c_begin;
                        this->sw_interval_state.back().is_line_number
                            = this->sw_line_number;
                    }
                    this->sw_interval_state.resize(this->sw_depth + 1);
                    this->sw_hier_nodes.push_back(
                        std::make_unique<hier_node>());
                    break;
                case DT_RCURLY:
                case DT_RSQUARE:
                case DT_RPAREN: {
                    auto term = this->flush_values();
                    if (this->sw_depth > 0) {
                        this->sw_depth -= 1;
                        this->append_child_node(term);
                        this->sw_interval_state.pop_back();
                        this->sw_hier_stage
                            = std::move(this->sw_hier_nodes.back());
                        this->sw_hier_nodes.pop_back();
                    }
                    this->sw_values.emplace_back(el);
                    break;
                }
                case DT_COMMA:
                    if (this->sw_depth > 0) {
                        auto term = this->flush_values();
                        this->append_child_node(term);
                    }
                    break;
                case DT_LINE:
                    this->sw_line_number += 1;
                    break;
                case DT_WHITE:
                    break;
                default:
                    this->sw_values.emplace_back(el);
                    break;
            }
        }
        this->flush_values();

        if (this->sw_hier_stage != nullptr) {
            this->sw_hier_stage->hn_parent = this->sw_hier_nodes.back().get();
            this->sw_hier_nodes.back()->hn_children.push_back(
                std::move(this->sw_hier_stage));
        }
        this->sw_hier_stage = std::move(this->sw_hier_nodes.back());
        this->sw_hier_nodes.pop_back();
        if (this->sw_hier_stage->hn_children.size() == 1
            && this->sw_hier_stage->hn_named_children.empty())
        {
            this->sw_hier_stage
                = std::move(this->sw_hier_stage->hn_children.front());
            this->sw_hier_stage->hn_parent = nullptr;
        }

        retval.m_sections_root = std::move(this->sw_hier_stage);
        retval.m_sections_tree = sections_tree_t(std::move(this->sw_intervals));

        return retval;
    }

private:
    struct element {
        element(data_token_t token, pcre_context& pc)
            : e_token(token), e_capture(*pc.all())
        {
        }

        element(data_token_t token, pcre_context::capture_t& cap)
            : e_token(token), e_capture(cap)
        {
        }

        data_token_t e_token;
        pcre_context::capture_t e_capture;
    };

    struct interval_state {
        nonstd::optional<file_off_t> is_start;
        size_t is_line_number{0};
        std::string is_name;
    };

    nonstd::optional<pcre_context::capture_t> flush_values()
    {
        nonstd::optional<pcre_context::capture_t> last_key;
        nonstd::optional<pcre_context::capture_t> retval;
        auto& pi = this->sw_scanner.get_input();

        if (!this->sw_values.empty()) {
            if (!this->sw_interval_state.back().is_start) {
                this->sw_interval_state.back().is_start
                    = this->sw_values.front().e_capture.c_begin;
                this->sw_interval_state.back().is_line_number
                    = this->sw_line_number;
            }
            retval = this->sw_values.back().e_capture;
        }
        for (const auto& el : this->sw_values) {
            switch (el.e_token) {
                case DT_SYMBOL:
                case DT_CONSTANT:
                case DT_WORD:
                case DT_QUOTED_STRING:
                    last_key = el.e_capture;
                    break;
                case DT_COLON:
                case DT_EQUALS:
                    if (last_key) {
                        this->sw_interval_state.back().is_name
                            = pi.get_substr(&last_key.value());
                        if (!this->sw_interval_state.back().is_name.empty()) {
                            this->sw_interval_state.back().is_start
                                = static_cast<ssize_t>(
                                    last_key.value().c_begin);
                            this->sw_interval_state.back().is_line_number
                                = this->sw_line_number;
                        }
                        last_key = nonstd::nullopt;
                    }
                    break;
                default:
                    break;
            }
        }

        this->sw_values.clear();

        return retval;
    }

    void append_child_node(nonstd::optional<pcre_context::capture_t> terminator)
    {
        auto& ivstate = this->sw_interval_state.back();
        if (!ivstate.is_start || !terminator) {
            return;
        }

        auto* top_node = this->sw_hier_nodes.back().get();
        auto new_key = ivstate.is_name.empty()
            ? lnav::document::section_key_t{top_node->hn_children.size()}
            : lnav::document::section_key_t{ivstate.is_name};
        this->sw_intervals.emplace_back(
            ivstate.is_start.value(),
            static_cast<ssize_t>(terminator.value().c_end),
            new_key);
        auto new_node = this->sw_hier_stage != nullptr
            ? std::move(this->sw_hier_stage)
            : std::make_unique<lnav::document::hier_node>();
        auto* retval = new_node.get();
        new_node->hn_parent = top_node;
        new_node->hn_start = this->sw_intervals.back().start;
        new_node->hn_line_number = ivstate.is_line_number;
        if (!ivstate.is_name.empty()) {
            top_node->hn_named_children.insert({
                ivstate.is_name,
                retval,
            });
        }
        top_node->hn_children.emplace_back(std::move(new_node));
        ivstate.is_start = nonstd::nullopt;
        ivstate.is_line_number = 0;
        ivstate.is_name.clear();
    }

    data_scanner sw_scanner;
    int sw_depth{0};
    size_t sw_line_number{0};
    std::vector<element> sw_values{};
    std::vector<interval_state> sw_interval_state;
    std::vector<lnav::document::section_interval_t> sw_intervals;
    std::vector<std::unique_ptr<lnav::document::hier_node>> sw_hier_nodes;
    std::unique_ptr<lnav::document::hier_node> sw_hier_stage;
};

metadata
discover_structure(string_fragment content)
{
    return structure_walker(content).walk();
}

std::vector<breadcrumb::possibility>
metadata::possibility_provider(const std::vector<section_key_t>& path)
{
    std::vector<breadcrumb::possibility> retval;
    auto curr_node = lnav::document::hier_node::lookup_path(
        this->m_sections_root.get(), path);
    if (curr_node) {
        auto* parent_node = curr_node.value()->hn_parent;

        if (parent_node != nullptr) {
            for (const auto& sibling : parent_node->hn_named_children) {
                retval.template emplace_back(sibling.first);
            }
        }
    }
    return retval;
}

}  // namespace document
}  // namespace lnav
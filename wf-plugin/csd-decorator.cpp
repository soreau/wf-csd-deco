/* This plugin is the interface between the decorator client and Wayfire. It has several functions:
 *
 * - When a new view is mapped, it notifies the decorator client via a custom protocol that a new decoration
 *   is required.
 * - When a new decoration toplevel is created, we attach it to the main view with several nodes:
 *   First, we attach the translation node, which has a child mask node, whose child is the decoration
 * surface.
 *   The translation node is responsible for setting the position of the decoration relative to the main view.
 *   The mask node cuts out the middle of the decoration so that transparent views remain transparent.
 *   The main decoration surface contains the actual decorations.
 *
 * - On each transaction involving a decorated view, the plugin adds a decoration object associated with the
 *   view to the transaction. The transaction object resizes the decoration on commit, and is ready when the
 *   decoration surface also resizes to the new size. Special care should be taken for the cases where the
 *   main view does not obey the compositor-requested size: in those cases, the decoration needs to be resized
 *   again to the final size of the main view.
 *
 *   Copyright © 2023 Ilia Bozhinov <ammen99@gmail.com>
 *   Copyright © 2026 Scott Moreau <oreaus@gmail.com>
 *
 */
#include <memory>
#include <wayfire/core.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/geometry.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/object.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/debug.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/window-manager.hpp>

#include <wayfire/toplevel.hpp>
#include <wayfire/txn/transaction-object.hpp>
#include <wayfire/txn/transaction-manager.hpp>

#include <type_traits>
#include <wayfire/util.hpp>
#include <wayfire/view.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/plugins/common/util.hpp>

#include <wayfire/signal-definitions.hpp>
#include "wf-decorator-protocol.h"

#include <wayfire/unstable/wlr-surface-node.hpp>
#include <wayfire/unstable/wlr-view-events.hpp>
#include <wayfire/unstable/translation-node.hpp>

void create_xdg_popup(wlr_xdg_popup *popup);


namespace wf
{
namespace csd_decorator
{
class csd_decoration_object_t;
}
}

class csd_toplevel_custom_data : public wf::custom_data_t
{
  public:
    std::shared_ptr<wf::csd_decorator::csd_decoration_object_t> decoration;
    wf::point_t margin_offset;
};

namespace wf
{
namespace csd_decorator
{
wf::decoration_margins_t deco_margins =
{
    .left   = 0,
    .right  = 0,
    .bottom = 0,
    .top    = 0,
};

class windowed_geometry_data_t : public wf::custom_data_t
{
  public:
    bool is_grabbed = false;

    /** Last geometry the view has had in non-tiled and non-fullscreen state.
     * -1 as width/height means that no such geometry has been stored. */
    wf::geometry_t last_windowed_geometry = {0, 0, -1, -1};

    /**
     * The workarea when last_windowed_geometry was stored. This is used
     * for ex. when untiling a view to determine its geometry relative to the
     * (potentially changed) workarea of its output.
     */
    wf::geometry_t windowed_geometry_workarea = {0, 0, -1, -1};
};

using decoration_node_t = std::shared_ptr<wf::scene::wlr_surface_node_t>;
std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;

wf::scene::damage_callback push_damage = [] (wf::regionf_t)
{};

void destroy_render_instance_manager()
{
    if (!instance_manager)
    {
        return;
    }

    instance_manager.reset();
    instance_manager = nullptr;
}

void create_render_instance_manager(std::vector<wf::scene::node_ptr> nodes, wf::output_t *output)
{
    if (instance_manager || !output)
    {
        return;
    }

    wf::regionf_t region;
    for (auto n : nodes)
    {
        region |= n->get_bounding_box();
    }

    instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage, output);
    instance_manager->set_visibility_region(region);
}

std::ostream& operator <<(std::ostream& out, const wf::dimensions_t& dims)
{
    out << dims.width << "x" << dims.height;
    return out;
}

static const std::string deco_transformer_name = "csd-deco-transformer";
using namespace wf::animation;
class deco_animation_t : public duration_t
{
  public:
    using duration_t::duration_t;
};

/**
 * A node which cuts out a part of its children (visually).
 */
class csd_mask_node_t : public wf::scene::floating_inner_node_t
{
  public:
    // The rendered part of the decoration which does not include the client buffer area
    wf::regionf_t allowed;

    csd_mask_node_t() : floating_inner_node_t(false)
    {}

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        if (allowed.contains_pointf(at))
        {
            return wf::scene::floating_inner_node_t::find_node_at(at);
        }

        return {};
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output) override
    {
        instances.push_back(std::make_unique<csd_mask_render_instance_t>(this, push_damage, output));
    }

    class csd_mask_render_instance_t : public wf::scene::render_instance_t
    {
        std::vector<wf::scene::render_instance_uptr> children;
        csd_mask_node_t *self;

      public:
        csd_mask_render_instance_t(csd_mask_node_t *self, wf::scene::damage_callback damage_cb,
            wf::output_t *output)
        {
            this->self = self;

            for (auto& ch : self->get_children())
            {
                if (ch->is_enabled())
                {
                    ch->gen_render_instances(children, damage_cb, output);
                }
            }
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, wf::regionf_t& damage) override
        {
            auto child_damage = (damage & self->allowed);
            for (auto& ch : children)
            {
                ch->schedule_instructions(instructions, target, child_damage);
            }
        }

        void presentation_feedback(wf::output_t *output) override
        {
            for (auto& ch : children)
            {
                ch->presentation_feedback(output);
            }
        }

        void compute_visibility(wf::output_t *output, wf::regionf_t& visible) override
        {
            for (auto& ch : children)
            {
                ch->compute_visibility(output, visible);
            }
        }
    };
};

static const std::string csd_decorator_prefix = "__wf_decorator:";
wl_resource *decorator_resource = NULL;
wl_listener deco_client_destroy_listener;
void select_window(uint32_t select_id);
void notify_focus(uint32_t focus_id);
void notify_tiled(uint32_t wf_id, uint32_t edges);
void ungroup_window(wl_client*, struct wl_resource*, uint32_t id, bool closing);

class csd_decoration_object_t : public wf::txn::transaction_object_t
{
    enum class csd_decoration_tx_state
    {
        // No transactions in flight
        STABLE,
        // Transaction has just started
        START,
        // The decoration client has ACKed our initial size request. However, the decorated toplevel's client
        // has not ACKed the request yet, so we do not know the actual 'final' size of the client.
        TENTATIVE,
        // Decorated toplevel has set its final size, waiting for the decoration to respond.
        WAITING_FINAL,
    };

  public:
    std::string stringify() const
    {
        std::ostringstream out;
        out << "csddeco(" << this << ")";
        return out.str();
    }

    void set_pending_size(wf::dimensions_t desired)
    {
        if (!toplevel)
        {
            return;
        }

        this->pending = desired;
    }

    void set_final_size(wf::dimensions_t final)
    {
        if (!toplevel)
        {
            return;
        }

        LOGD("Final size is ", final, " state is ", (int)deco_state);

        if (this->committed == final)
        {
            switch (this->deco_state)
            {
              case csd_decoration_tx_state::STABLE:
                break;

              case csd_decoration_tx_state::START:
                this->deco_state = csd_decoration_tx_state::TENTATIVE;
                wf::scene::update(target_view->get_root_node(), wf::scene::update_flag::REFOCUS);

                break;

              case csd_decoration_tx_state::WAITING_FINAL:
                break;

              case csd_decoration_tx_state::TENTATIVE:
                this->deco_state = csd_decoration_tx_state::WAITING_FINAL;
                wf::txn::emit_object_ready(this);
                return;
            }
        }

        this->committed = final;
        wlr_xdg_toplevel_set_size(toplevel, final.width, final.height);

        this->deco_state = csd_decoration_tx_state::WAITING_FINAL;
    }

    void size_updated()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        wlr_box box = toplevel->base->geometry;

        LOGD("Size is ", wf::dimensions(box), " state is ", (int)deco_state);

        auto vg = wf::toplevel_cast(target_view)->get_geometry();

        switch (this->deco_state)
        {
          case csd_decoration_tx_state::STABLE:
            // Client simply committed, nothing has changed

            return;

          case csd_decoration_tx_state::TENTATIVE:
            // Client commits twice?
            if (wf::dimensions(box) != committed)
            {
                LOGD(wf::dimensions(box), " != ", committed);
                committed = wf::dimensions(box);
            }

            wlr_xdg_toplevel_set_size(toplevel, vg.width, vg.height);

            return;

          case csd_decoration_tx_state::START:
            deco_state = csd_decoration_tx_state::TENTATIVE;
            return;

          case csd_decoration_tx_state::WAITING_FINAL:
            deco_state = csd_decoration_tx_state::STABLE;
            if (wf::dimensions(box) != committed)
            {
                adjust_target_geometry();
            }

            wf::txn::emit_object_ready(this);
            break;
        }

        wf::scene::update(target_view->get_root_node(), wf::scene::update_flag::REFOCUS);
    }

    void commit()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        set_pending_size(wf::dimensions(decorated_toplevel->pending().geometry));

        deco_state = csd_decoration_tx_state::START;

        LOGD("Committing with ", pending, " state is ", (int)deco_state);
        recompute_mask();

        wlr_box box = toplevel->base->geometry;

        if (wf::dimensions(box) == pending)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        if (on_commit.is_connected())
        {
            on_commit.emit(nullptr);
        }

        if (target_view->get_wlr_surface() &&
            wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()))
        {
            wlr_xdg_toplevel_set_size(toplevel, pending.width, pending.height);
        }
    }

    void apply()
    {
        if (toplevel)
        {
            pending_state.merge_state(toplevel->base->surface);
        }

        deco_node->apply_state(std::move(pending_state));
        recompute_mask();
    }

    void adjust_target_geometry()
    {
        auto desired = wf::dimensions(toplevel->base->geometry);
        auto vg = wf::toplevel_cast(target_view)->get_geometry();
        auto tg = wf::dimensions(vg);
        if (!target_view->get_wlr_surface())
        {
            return;
        }

        desired.width  = std::max(tg.width, desired.width);
        desired.height = std::max(tg.height, desired.height);
        desired.width  = std::max(desired.width, wf::toplevel_cast(
            target_view)->toplevel()->get_min_size().width);
        desired.height = std::max(desired.height, wf::toplevel_cast(
            target_view)->toplevel()->get_min_size().height);

        if (desired != tg)
        {
            LOGD("Adjusting target on deco commit: ", desired, " != ", tg);
            if (wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()))
            {
                desired.height -= margin_top - margin_bottom + 1;
                auto vg = wf::toplevel_cast(target_view)->get_geometry();
                wlr_xwayland_surface_configure(wlr_xwayland_surface_try_from_wlr_surface(target_view->
                    get_wlr_surface()),
                    vg.x, vg.y, desired.width, desired.height);
            } else
            {
                wlr_xdg_toplevel_set_size(wlr_xdg_toplevel_try_from_wlr_surface(target_view->
                    get_wlr_surface()),
                    desired.width, desired.height);
            }
        }
    }

    std::shared_ptr<wf::toplevel_t> decorated_toplevel;

  public:
    csd_decoration_object_t(
        wlr_xdg_toplevel *toplevel, wayfire_view target_view, decoration_node_t deco_node,
        std::weak_ptr<csd_mask_node_t> mask, std::shared_ptr<wf::toplevel_t> decorated_toplevel,
        std::shared_ptr<wf::scene::translation_node_t> root_node)
    {
        this->toplevel    = toplevel;
        this->deco_node   = deco_node;
        this->target_view = target_view;
        this->mask_node   = mask;
        this->decorated_toplevel = decorated_toplevel;
        this->root_node = root_node;
        this->margin_offset.x = this->margin_offset.y = -1;
        /* TODO: Make duration configurable */
        this->progression = deco_animation_t(wf::create_option<int>(400));

        on_commit.set_callback([=] (void*)
        {
            if (!target_view->is_mapped())
            {
                return;
            }

            if (toplevel)
            {
                pending_state.merge_state(toplevel->base->surface);
            }

            if ((deco_state == csd_decoration_tx_state::STABLE) ||
                (deco_state == csd_decoration_tx_state::TENTATIVE))
            {
                deco_node->apply_state(std::move(pending_state));
                recompute_mask();
            }

            if (deco_state != csd_decoration_tx_state::TENTATIVE)
            {
                size_updated();
            } else
            {
                auto vg = wf::toplevel_cast(target_view)->get_geometry();
                wlr_xdg_toplevel_set_size(toplevel, vg.width, vg.height);
            }
        });

        on_deco_destroy.set_callback([=] (void*)
        {
            handle_destroy();
        });

        on_target_destroy.set_callback([=] (void*)
        {
            handle_destroy();
        });

        on_request_move.set_callback([=] (void*)
        {
            wf::get_core().default_wm->move_request(wf::toplevel_cast(target_view));
        });

        on_request_resize.set_callback([=] (void *data)
        {
            auto ev = static_cast<wlr_xdg_toplevel_resize_event*>(data);
            wf::get_core().default_wm->resize_request(wf::toplevel_cast(target_view), ev->edges);
        });

        on_request_deco_maximize.set_callback([=] (void*)
        {
            auto edges = wf::toplevel_cast(target_view)->pending_tiled_edges();
            wf::get_core().default_wm->tile_request(
                wf::toplevel_cast(target_view),
                edges ? 0 : wf::TILED_EDGES_ALL);
            send_tiled_edges();
        });

        on_request_target_maximize.set_callback([=] (void*)
        {
            send_tiled_edges();
        });

        on_request_minimize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->minimize_request(wf::toplevel_cast(target_view),
                !wf::toplevel_cast(target_view)->minimized);
        });

        on_new_popup.set_callback([=] (void *data)
        {
            auto popup = (wlr_xdg_popup*)data;

            if (!popup)
            {
                return;
            }

            if (deco_node->get_surface() != popup->parent)
            {
                return;
            }

            popup->parent = target_view->get_wlr_surface();
            on_map_popup.connect(&popup->base->surface->events.map);
            on_destroy_popup.connect(&popup->base->surface->events.destroy);
            create_xdg_popup(popup);
            current_popup = popup;
        });

        on_map_popup.set_callback([=] (void*)
        {
            auto popup = current_popup;
            popup->base->pending.geometry.y += margin_top - margin_bottom - 5;
            popup->base->current.geometry.y  = popup->base->pending.geometry.y;
        });

        on_destroy_popup.set_callback([=] (void*)
        {
            on_map_popup.disconnect();
            on_destroy_popup.disconnect();
        });

        on_request_move.connect(&toplevel->events.request_move);
        on_request_resize.connect(&toplevel->events.request_resize);
        on_request_deco_maximize.connect(&toplevel->events.request_maximize);
        target_view->connect(&on_view_tiled);
        target_view->connect(&on_fullscreen);
        target_view->connect(&on_view_activated);
        target_view->connect(&on_view_minimized);
        target_view->connect(&on_target_unmapped);
        target_view->connect(&on_view_title_changed);
        target_view->connect(&on_view_focus_request);
        if (target_view->get_output())
        {
            target_view->get_output()->connect(&on_request_target_move);
        }

        on_request_minimize.connect(&toplevel->events.request_minimize);
        on_new_popup.connect(&wlr_xdg_surface_try_from_wlr_surface(
            deco_node->get_surface())->client->shell->events.new_popup);
        on_commit.connect(&toplevel->base->surface->events.commit);
        on_deco_destroy.connect(&toplevel->events.destroy);
        if (target_view->get_wlr_surface() &&
            wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface()))
        {
            on_request_target_maximize.connect(&wlr_xdg_toplevel_try_from_wlr_surface(target_view->
                get_wlr_surface())->events.request_maximize);
            on_target_destroy.connect(&wlr_xdg_toplevel_try_from_wlr_surface(
                target_view->get_wlr_surface())->events.destroy);
        }

        if (wf::toplevel_cast(target_view)->toplevel()->pending().fullscreen)
        {
            wf::scene::remove_child(root_node);
        } else
        {
            wf::scene::add_front(target_view->get_surface_root_node(), root_node);
        }

        if (wf::get_core().seat->get_active_view() == target_view)
        {
            notify_focus(target_view->get_id());
        }

        send_tiled_edges();
    }

    void handle_destroy()
    {
        unset_hook(target_view->get_output());
        ungroup_window(NULL, NULL, target_view->get_id(), false);
        if (decorator_resource)
        {
            wf_decorator_manager_send_destroy_decoration(decorator_resource, target_view->get_id());
        }

        on_commit.disconnect();
        on_deco_destroy.disconnect();
        on_target_destroy.disconnect();
        on_target_unmapped.disconnect();
        on_new_popup.disconnect();
        on_map_popup.disconnect();
        on_destroy_popup.disconnect();
        on_request_move.disconnect();
        on_request_resize.disconnect();
        on_request_minimize.disconnect();
        on_request_deco_maximize.disconnect();
        on_request_target_maximize.disconnect();
        on_fullscreen.disconnect();
        on_view_tiled.disconnect();
        on_view_minimized.disconnect();
        on_view_activated.disconnect();
        on_view_title_changed.disconnect();
        on_view_focus_request.disconnect();
        on_request_target_move.disconnect();

        this->toplevel = nullptr;
    }

    wf::signal::connection_t<wf::view_unmapped_signal> on_target_unmapped = [=] (wf::view_unmapped_signal*)
    {
        handle_destroy();
    };

    void sync_group_tiled_states(uint32_t edges)
    {
        if (!group_id)
        {
            return;
        }

        wf::geometry_t target_geometry;
        if (edges)
        {
            target_geometry = wf::toplevel_cast(target_view)->get_geometry();
            auto test_geometry =
                wf::get_core().default_wm->get_last_windowed_geometry(wf::toplevel_cast(target_view)).
                    value_or({0, 0, -1, -1});
            if (test_geometry.width == -1)
            {
                wf::get_core().default_wm->update_last_windowed_geometry(wf::toplevel_cast(target_view));
            }
        }

        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (data && data->decoration)
            {
                data->decoration->on_view_tiled.disconnect();
            }
        }

        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (!data || !data->decoration)
            {
                continue;
            }

            if (data->decoration->group_id != group_id)
            {
                continue;
            }

            if ((v != target_view) && (edges != wf::toplevel_cast(v)->pending_tiled_edges()))
            {
                auto vg = wf::toplevel_cast(v)->get_geometry();
                if (edges)
                {
                    auto test_geometry =
                        wf::get_core().default_wm->get_last_windowed_geometry(wf::toplevel_cast(v)).
                            value_or({0, 0, -1, -1});
                    if (test_geometry.width == -1)
                    {
                        auto windowed = v->get_data_safe<windowed_geometry_data_t>();
                        windowed->last_windowed_geometry = target_geometry;
                    }
                } else
                {
                    target_geometry =
                        wf::get_core().default_wm->get_last_windowed_geometry(wf::toplevel_cast(v)).
                            value_or(vg);
                }

                wf::view_tiled_signal data;
                data.view = wf::toplevel_cast(v);
                data.old_edges = wf::toplevel_cast(v)->toplevel()->pending().tiled_edges;
                data.new_edges = edges;

                wf::toplevel_cast(v)->toplevel()->pending().tiled_edges = edges;
                v->emit(&data);
                wf::toplevel_cast(v)->set_geometry(target_geometry);
            }
        }

        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (data && data->decoration)
            {
                data->decoration->target_view->connect(&data->decoration->on_view_tiled);
            }
        }

        send_tiled_edges();
    }

    wf::signal::connection_t<wf::view_move_request_signal> on_request_target_move =
        [=] (wf::view_move_request_signal*)
    {
        if (!group_id)
        {
            return;
        }

        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (!data || !data->decoration)
            {
                continue;
            }

            if (data->decoration->group_id != group_id)
            {
                continue;
            }

            if (wf::toplevel_cast(v)->pending_tiled_edges())
            {
                continue;
            }

            if (v != target_view)
            {
                wf::get_core().default_wm->update_last_windowed_geometry(wf::toplevel_cast(v));
            }
        }
    };

    void send_tiled_edges()
    {
        auto edges = wf::toplevel_cast(target_view)->pending_tiled_edges();
        notify_tiled(target_view->get_id(), edges);
    }

    wf::signal::connection_t<wf::view_title_changed_signal> on_view_title_changed =
        [=] (wf::view_title_changed_signal*)
    {
        wf_decorator_manager_send_title_changed(decorator_resource,
            target_view->get_id(), target_view->get_title().c_str());
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_view_tiled = [=] (wf::view_tiled_signal *ev)
    {
        sync_group_tiled_states(ev->new_edges);
    };

    wf::signal::connection_t<wf::view_minimized_signal> on_view_minimized =
        [=] (wf::view_minimized_signal*)
    {
        /*
         * The following code is for grouped windows only
         */
        if (!group_id)
        {
            return;
        }

        /* First, disconnect all the view_minimized signals for all views */
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (data && data->decoration)
            {
                data->decoration->on_view_minimized.disconnect();
            }
        }

        /* Then, synchronize minimize and adjust enabled states */
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (!data || !data->decoration)
            {
                continue;
            }

            if (data->decoration->group_id != group_id)
            {
                continue;
            }

            if (v != target_view)
            {
                wf::scene::set_node_enabled(v->get_root_node(), wf::toplevel_cast(target_view)->minimized);
                wf::toplevel_cast(v)->set_minimized(wf::toplevel_cast(target_view)->minimized);
            }
        }

        if (!wf::toplevel_cast(target_view)->minimized)
        {
            wf::scene::set_node_enabled(target_view->get_root_node(), false);
        }

        /* Finally, reconnect the view_minimized signals for all views */
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (data && data->decoration)
            {
                data->decoration->target_view->connect(&data->decoration->on_view_minimized);
            }
        }
    };

    wf::signal::connection_t<wf::view_focus_request_signal> on_view_focus_request =
        [=] (wf::view_focus_request_signal*)
    {
        /*
         * The following code is for grouped windows only
         */
        if (!group_id)
        {
            return;
        }

        /* Adjust enabled states */
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (!data || !data->decoration)
            {
                continue;
            }

            if (data->decoration->group_id != group_id)
            {
                continue;
            }

            if (v == target_view)
            {
                while (!v->get_root_node()->is_enabled())
                {
                    wf::scene::set_node_enabled(v->get_root_node(), true);
                }
            } else
            {
                while (v->get_root_node()->is_enabled())
                {
                    wf::scene::set_node_enabled(v->get_root_node(), false);
                }
            }
        }
    };

    wf::signal::connection_t<wf::view_activated_state_signal> on_view_activated =
        [=] (wf::view_activated_state_signal*)
    {
        if (wf::get_core().seat->get_active_view() == target_view)
        {
            notify_focus(target_view->get_id());
        }
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen =
        [=] (wf::view_fullscreen_signal *ev)
    {
        if (ev->view != target_view)
        {
            return;
        }

        if (ev->state)
        {
            wf::scene::remove_child(root_node);
        } else
        {
            wf::scene::readd_front(target_view->get_surface_root_node(), root_node);
        }
    };

    wf::effect_hook_t animation_driver_hook = [=] ()
    {
        target_view->damage();

        auto tr = wf::ensure_named_transformer<wf::scene::view_2d_transformer_t>(
            target_view, wf::TRANSFORMER_2D, deco_transformer_name, target_view);

        auto progress = progression.progress();
        target_view->get_transformed_node()->begin_transform_update();
        tr->alpha = 1.0 - progress;
        if (group_id)
        {
            tr->translation_x = (from_geometry.x - to_geometry.x) * (1.0 - progress);
            tr->translation_y = (from_geometry.y - to_geometry.y) * (1.0 - progress);
        } else
        {
            tr->translation_x = (from_geometry.x - to_geometry.x) * progress;
            tr->translation_y = (from_geometry.y - to_geometry.y) * progress;
        }

        target_view->get_transformed_node()->end_transform_update();

        target_view->damage();

        if (!progression.running())
        {
            unset_hook(target_view->get_output());

            if (group_id)
            {
                while (target_view->get_root_node()->is_enabled())
                {
                    wf::scene::set_node_enabled(target_view->get_root_node(), false);
                    wf::scene::set_node_enabled(target_view->get_root_node(), false);
                }
            }
        }
    };

    void set_hook(wf::output_t *output, wf::geometry_t from_geometry, wf::geometry_t to_geometry)
    {
        if (!output)
        {
            return;
        }

        if (!hook_set)
        {
            output->render->add_effect(&animation_driver_hook,
                wf::OUTPUT_EFFECT_PRE);
        }

        hook_set = true;

        this->from_geometry = from_geometry;
        this->to_geometry   = to_geometry;

        if (group_id)
        {
            if (!progression.get_direction())
            {
                progression.reverse();
            }

            progression.start();
        } else
        {
            if (progression.running())
            {
                progression.reverse();
            } else
            {
                if (progression.get_direction())
                {
                    progression.reverse();
                }

                progression.start();
            }
        }
    }

    void unset_hook(wf::output_t *output)
    {
        if (!hook_set || !output)
        {
            return;
        }

        hook_set = false;

        output->render->rem_effect(&animation_driver_hook);
        target_view->get_transformed_node()->rem_transformer(deco_transformer_name);
    }

    void set_margins(int top, int bottom, int left, int right, int border, wf::point_t offset)
    {
        this->margin_top    = top;
        this->margin_bottom = bottom;
        this->margin_left   = left;
        this->margin_right  = right;
        this->margin_border = border;
        this->margin_offset = offset;
    }

    uint32_t group_id = 0;
    bool use_csd = false;
    bool borders_set = false;
    wayfire_view target_view;
    decoration_node_t deco_node;
    wf::pointf_t ungroup_restore_position;
    std::weak_ptr<csd_mask_node_t> mask_node;
    std::shared_ptr<wf::scene::translation_node_t> root_node;

  private:
    wf::dimensions_t pending = {0, 0};
    wf::dimensions_t committed = {0, 0};

    void recompute_mask()
    {
        auto masked = mask_node.lock();
        if (!masked)
        {
            LOGD("Masked node does not exist anymore??");
            return;
        }

        auto bbox = deco_node->get_bounding_box();

        masked->allowed = bbox;
        wf::geometry_t cut_out = wf::geometry_t{
            .x     = bbox.x + margin_left,
            .y     = bbox.y + margin_top,
            .width = bbox.width - margin_left * 2,
            .height = bbox.height - margin_top - margin_left - margin_bottom,
        };
        masked->allowed ^= cut_out;
    }

    double margin_left = 0;
    double margin_top = 0;
    double margin_right = 0;
    double margin_bottom = 0;
    double margin_border = 0;
    wf::point_t margin_offset;

    wf::scene::surface_state_t pending_state;

    wlr_xdg_toplevel *toplevel;

    bool hook_set = false;
    wlr_xdg_popup *current_popup;
    deco_animation_t progression;
    wf::geometry_t from_geometry, to_geometry;

    wf::wl_listener_wrapper on_commit, on_new_popup, on_map_popup, on_destroy_popup, on_deco_destroy,
        on_target_destroy;
    wf::wl_listener_wrapper on_request_move, on_request_resize, on_request_minimize;
    wf::wl_listener_wrapper on_request_deco_maximize, on_request_target_maximize;
    csd_decoration_tx_state deco_state = csd_decoration_tx_state::STABLE;
};

void do_update_borders(wl_client*, struct wl_resource*, uint32_t id, uint32_t top, uint32_t bottom,
    uint32_t left, uint32_t right, uint32_t border)
{
    wayfire_view target = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->get_id() == id)
        {
            target = v;
            break;
        }
    }

    if (!target)
    {
        return;
    }

    auto data = wf::toplevel_cast(target)->toplevel()->get_data<csd_toplevel_custom_data>();
    if (!data || !data->decoration)
    {
        return;
    }

    LOGD("do_update_borders: ", top, ", ", bottom, ", ", left, ", ", right);

    int l = left, t = top;
    bool use_csd = data->decoration->use_csd;
    LOGI(use_csd);

    deco_margins.top    = top - left + border;
    deco_margins.bottom = right;
    deco_margins.left   = right;
    deco_margins.right  = right;
    data->decoration->set_margins(top, bottom, left, right, border, data->margin_offset);

    auto edges = wf::toplevel_cast(data->decoration->target_view)->pending_tiled_edges();
    if (edges == wf::TILED_EDGES_ALL)
    {
        data->decoration->root_node->set_offset({-l, -t});
    } else if (edges)
    {
        data->decoration->root_node->set_offset(
            {use_csd ? -(l - data->margin_offset.x - data->margin_offset.x / 2 + 1) : -l,
                use_csd ? -(t - data->margin_offset.y - data->margin_offset.y / 2 - 2) : -t});
    } else
    {
        data->decoration->root_node->set_offset({use_csd ? -(l - data->margin_offset.x) : -l,
            use_csd ? -(t - data->margin_offset.y) : -t});
    }

    wf::get_core().tx_manager->schedule_object(wf::toplevel_cast(data->decoration->target_view)->toplevel());
    wf::scene::update(data->decoration->target_view->get_root_node(), 0xFF);
    wf::scene::update(data->decoration->root_node, 0xFF);
    wf::scene::damage_node(data->decoration->root_node, data->decoration->root_node->get_bounding_box());
}

void do_group_windows(wl_client*, struct wl_resource*, uint32_t parent_id, uint32_t child_id)
{
    uint32_t group_id = 1;
    wayfire_view parent = nullptr, child = nullptr;

    ungroup_window(NULL, NULL, child_id, false);

    std::vector<wf::scene::node_ptr> nodes;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == parent_id)
        {
            parent = v;
        }

        if (v->get_id() == child_id)
        {
            child = v;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
        if (data && data->decoration)
        {
            if (data->decoration->group_id >= group_id)
            {
                group_id = data->decoration->group_id + 1;
            }

            nodes.push_back(v->get_root_node());
        }
    }

    if (!parent || !child)
    {
        return;
    }

    auto parent_data = wf::toplevel_cast(parent)->toplevel()->get_data<csd_toplevel_custom_data>();
    auto child_data  = wf::toplevel_cast(child)->toplevel()->get_data<csd_toplevel_custom_data>();

    if (!parent_data || !child_data || !parent_data->decoration || !child_data->decoration)
    {
        return;
    }

    destroy_render_instance_manager();
    create_render_instance_manager(nodes, parent->get_output());

    if (!parent_data->decoration->group_id)
    {
        parent_data->decoration->group_id = child_data->decoration->group_id = group_id;
    } else
    {
        child_data->decoration->group_id = parent_data->decoration->group_id;
    }

    while (!parent->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(parent->get_root_node(), true);
    }

    auto cg = wf::toplevel_cast(child)->get_geometry();
    child_data->decoration->ungroup_restore_position = {cg.x, cg.y};
    auto vg = wf::toplevel_cast(parent)->get_geometry();
    parent_data->decoration->ungroup_restore_position = {vg.x, vg.y};
    wf::toplevel_cast(child)->move(vg.x, vg.y);
    auto from_geometry = cg;
    auto to_geometry   = wf::toplevel_cast(child)->get_geometry();
    if (from_geometry != to_geometry)
    {
        child_data->decoration->set_hook(child->get_output(), from_geometry, to_geometry);
    }

    wf::get_core().seat->focus_view(parent);
}

void notify_focus(uint32_t focus_id)
{
    if (decorator_resource)
    {
        wf_decorator_manager_send_notify_focus(decorator_resource, focus_id);
    }
}

void notify_tiled(uint32_t wf_id, uint32_t edges)
{
    if (decorator_resource)
    {
        wf_decorator_manager_send_notify_tiled(decorator_resource, wf_id, edges);
    }
}

void select_window(uint32_t select_id)
{
    wayfire_view view = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == select_id)
        {
            view = v;
            break;
        }
    }

    if (!view)
    {
        return;
    }

    auto view_data = wf::toplevel_cast(view)->toplevel()->get_data<csd_toplevel_custom_data>();

    if (!view_data || !view_data->decoration)
    {
        return;
    }

    while (!view->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(view->get_root_node(), true);
    }

    wf::get_core().default_wm->focus_raise_view(view);

    auto group_id = view_data->decoration->group_id;

    if (!group_id)
    {
        return;
    }

    view_data->decoration->unset_hook(view->get_output());

    for (auto& v : wf::get_core().get_all_views())
    {
        if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == view))
        {
            continue;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
        if (data && data->decoration)
        {
            if (data->decoration->group_id == group_id)
            {
                while (v->get_root_node()->is_enabled())
                {
                    wf::scene::set_node_enabled(v->get_root_node(), false);
                    wf::scene::set_node_enabled(v->get_root_node(), false);
                }
            }
        }
    }
}

void do_select_window(wl_client*, struct wl_resource*, uint32_t select_id)
{
    select_window(select_id);
}

void ungroup_window(wl_client*, struct wl_resource*, uint32_t id, bool restore_position)
{
    wayfire_view view = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == id)
        {
            view = v;
            break;
        }
    }

    if (!view)
    {
        return;
    }

    auto view_data = wf::toplevel_cast(view)->toplevel()->get_data<csd_toplevel_custom_data>();

    if (!view_data || !view_data->decoration)
    {
        return;
    }

    auto rg = view_data->decoration->ungroup_restore_position;
    view_data->decoration->ungroup_restore_position = {0, 0};

    auto group_id = view_data->decoration->group_id;
    view_data->decoration->group_id = 0;

    if (!group_id)
    {
        return;
    }

    auto from_geometry = wf::toplevel_cast(view)->get_geometry();

    if (restore_position)
    {
        wf::toplevel_cast(view)->move(rg.x, rg.y);
    }

    while (!view->get_root_node()->is_enabled())
    {
        wf::scene::set_node_enabled(view->get_root_node(), true);
    }

    wf::get_core().default_wm->focus_raise_view(view);

    auto to_geometry = wf::toplevel_cast(view)->get_geometry();
    if (from_geometry != to_geometry)
    {
        view_data->decoration->set_hook(view->get_output(), from_geometry, to_geometry);
    }

    bool visible_view_found = false;
    wayfire_view unhide_me  = nullptr;
    std::vector<wf::scene::node_ptr> nodes;
    uint64_t last_group_focused_timestamp = 0;
    for (auto& v : wf::get_core().get_all_views())
    {
        if ((v->role != wf::VIEW_ROLE_TOPLEVEL) || (v == view))
        {
            continue;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
        if (data && data->decoration)
        {
            if (data->decoration->group_id == group_id)
            {
                if (v->get_root_node()->is_enabled())
                {
                    visible_view_found = true;
                }

                if (wf::get_focus_timestamp(v) > last_group_focused_timestamp)
                {
                    last_group_focused_timestamp = wf::get_focus_timestamp(v);
                    unhide_me = v;
                }
            }

            nodes.push_back(v->get_root_node());
        }
    }

    destroy_render_instance_manager();
    create_render_instance_manager(nodes, view->get_output());

    if (unhide_me && !visible_view_found)
    {
        while (!unhide_me->get_root_node()->is_enabled())
        {
            wf::scene::set_node_enabled(unhide_me->get_root_node(), true);
        }
    }
}

void do_ungroup_window(wl_client*, struct wl_resource*, uint32_t id)
{
    ungroup_window(NULL, NULL, id, true);
}

void do_close_request(wl_client*, struct wl_resource*, uint32_t id)
{
    wayfire_view view = nullptr;
    for (auto& v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        if (v->get_id() == id)
        {
            view = v;
            break;
        }
    }

    if (!view)
    {
        return;
    }

    view->close();
}

const struct wf_decorator_manager_interface decorator_implementation =
{
    .update_borders = do_update_borders,
    .group_windows  = do_group_windows,
    .select_window  = do_select_window,
    .ungroup_window = do_ungroup_window,
    .close_request  = do_close_request
};

static wl_client *decorator_client;
static void unbind_decorator(wl_resource*)
{
    LOGD("Unbinding wf-decorator");
    decorator_resource = NULL;
}

static void handle_deco_client_destroy(struct wl_listener *listener, void*)
{
    LOGD("handle_deco_client_destroy");
    if (decorator_resource)
    {
        wl_list_remove(&deco_client_destroy_listener.link);

        for (auto & v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            ungroup_window(NULL, NULL, v->get_id(), true);
        }
    }

    for (auto & v : wf::get_core().get_all_views())
    {
        if (v->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            continue;
        }

        auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
        if (!data || !data->decoration)
        {
            continue;
        }

        data->decoration->handle_destroy();

        auto deco_node = data->decoration->deco_node;
        if (deco_node)
        {
            wf::scene::remove_child(deco_node);
            deco_node.reset();
        }

        auto root_node = data->decoration->root_node;
        if (root_node)
        {
            wf::scene::remove_child(root_node);
            root_node.reset();
        }

        data->decoration.reset();

        wf::get_core().tx_manager->schedule_object(wf::toplevel_cast(v)->toplevel());

        v->damage();
    }

    for (auto & output : wf::get_core().output_layout->get_outputs())
    {
        output->render->damage_whole();
    }

    unbind_decorator(NULL);
    if (listener)
    {
        decorator_client = NULL;
    }
}

wf::option_wrapper_t<bool> decorate_csd{"csd-decorator/decorate_csd"};
wf::view_matcher_t ignore_views_match{"csd-decorator/ignore_views"};
wf::option_wrapper_t<std::string> ignore_views_as_string{"csd-decorator/ignore_views"};

static bool should_be_decorated(wayfire_view view)
{
    if (ignore_views_match.matches(view))
    {
        return false;
    }

    if (decorate_csd)
    {
        return true;
    }

    if (!decorate_csd && !wf::toplevel_cast(view)->should_be_decorated())
    {
        return false;
    }

    return true;
}

void bind_decorator(wl_client *client, void*, uint32_t, uint32_t id)
{
    if (decorator_resource)
    {
        if (client != decorator_client)
        {
            wl_client_destroy(client);
        }

        return;
    }

    LOGI("Binding wf-decorator");
    auto resource = wl_resource_create(client, &wf_decorator_manager_interface, 1, id);
    decorator_client = client;

    wl_resource_set_implementation(resource, &decorator_implementation, NULL, NULL);
    decorator_resource = resource;

    deco_client_destroy_listener.notify = handle_deco_client_destroy;
    wl_client_add_destroy_listener(client, &deco_client_destroy_listener);

    for (auto & view : wf::get_core().get_all_views())
    {
        if (view->get_app_id() == "org.wf.sample-decorator")
        {
            continue;
        }

        if ((view->role != wf::VIEW_ROLE_TOPLEVEL) || !view->is_mapped())
        {
            continue;
        }

        if (!should_be_decorated(view))
        {
            continue;
        }

        wlr_server_decoration_manager_set_default_mode(
            wf::get_core().protocols.decorator_manager,
            WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
        wf_decorator_manager_send_create_new_decoration(decorator_resource, view->get_id());

        auto data = wf::toplevel_cast(view)->toplevel()->get_data_safe<csd_toplevel_custom_data>();

        if ((data->margin_offset.x == -1) && (data->margin_offset.y == -1))
        {
            auto bg = view->get_bounding_box();
            auto vg = wf::toplevel_cast(view)->get_geometry();
            data->margin_offset.x = vg.x - bg.x;
            data->margin_offset.y = vg.y - bg.y;
        }

        LOGD("margin_offsets: ", data->margin_offset.x, ",", data->margin_offset.y);
    }
}

class csd_decoration_plugin : public wf::plugin_interface_t
{
  public:
    wl_global *decorator_global;

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_view_geometry_changed =
        [=] (wf::view_geometry_changed_signal *ev)
    {
        if ((ev->view->role != wf::VIEW_ROLE_TOPLEVEL) || !ev->view->is_mapped())
        {
            return;
        }

        auto data = wf::toplevel_cast(ev->view)->toplevel()->get_data<csd_toplevel_custom_data>();

        if (!data || !data->decoration)
        {
            return;
        }

        auto group_id = data->decoration->group_id;

        if (!group_id)
        {
            return;
        }

        on_view_geometry_changed.disconnect();
        auto vg = wf::toplevel_cast(ev->view)->get_geometry();

        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->role != wf::VIEW_ROLE_TOPLEVEL)
            {
                continue;
            }

            auto cdata = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
            if (!cdata || !cdata->decoration)
            {
                continue;
            }

            if (cdata->decoration->group_id != group_id)
            {
                continue;
            }

            if (wf::toplevel_cast(ev->view)->pending_tiled_edges())
            {
                continue;
            }

            wf::toplevel_cast(v)->move(vg.x, vg.y);
        }

        wf::get_core().connect(&on_view_geometry_changed);
    };

    void init_decor(wayfire_view view, wlr_surface *surface)
    {
        LOGD("Got decorator view ", view->get_title());

        auto id_str = std::string(view->get_title()).substr(csd_decorator_prefix.length());
        auto id     = std::stoul(id_str.c_str());

        wayfire_toplevel_view target;
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->get_id() == id)
            {
                target = toplevel_cast(v);
                break;
            }
        }

        auto deco_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);

        if (!target)
        {
            LOGD("View is gone already?");
            view->close();
            return;
        }

        if (!target->toplevel())
        {
            LOGD("View does not support toplevel interface?");
            view->close();
            return;
        }

        if (!surface)
        {
            LOGD("Premap wlr_surface is null?");
            return;
        }

        if (!deco_toplevel)
        {
            LOGD("View is not an xdg_toplevel?");
            return;
        }

        auto data = target->toplevel()->get_data_safe<csd_toplevel_custom_data>();

        auto decoration_root_node = std::make_shared<wf::scene::translation_node_t>();
        auto mask_node = std::make_shared<csd_mask_node_t>();
        decoration_root_node->set_children_list({mask_node});

        auto deco_surf = std::make_shared<wf::scene::wlr_surface_node_t>(surface, false);
        data->decoration = std::make_shared<csd_decoration_object_t>(
            deco_toplevel, target, deco_surf, mask_node,
            target->toplevel(), decoration_root_node);
        data->decoration->use_csd = !target->should_be_decorated();
        mask_node->set_children_list({deco_surf});

        target->toplevel()->connect(&on_object_ready);
        // Trigger a new transaction to set margins
        wf::get_core().tx_manager->schedule_object(target->toplevel());

        wf_decorator_manager_send_title_changed(decorator_resource, id, target->get_title().c_str());
        wf_decorator_manager_send_app_id_changed(decorator_resource, id, target->get_app_id().c_str());

        /* Nudge so the client computes and sends the decorator window shadow margins */
        auto vg = target->get_geometry();
        wlr_xdg_toplevel_set_size(deco_toplevel, vg.width + 1, vg.height);
        wlr_xdg_toplevel_set_size(deco_toplevel, vg.width, vg.height + 1);
    }

    wf::signal::connection_t<wf::view_pre_map_signal> on_pre_map = [=] (wf::view_pre_map_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        if (ev->view->get_app_id() == "org.wf.sample-decorator")
        {
            ev->override_implementation = true;
            init_decor(ev->view, ev->surface);
            wlr_server_decoration_manager_set_default_mode(
                wf::get_core().protocols.decorator_manager,
                WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
            return;
        }
    };

    wf::signal::connection_t<wf::view_mapped_signal> on_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        if (ev->view->get_app_id() == "org.wf.sample-decorator")
        {
            return;
        }

        if (ev->view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            LOGD("Not a toplevel");
            return;
        }

        if (wf::toplevel_cast(ev->view)->toplevel()->get_data<csd_toplevel_custom_data>())
        {
            LOGD("Already has decoration");
            return;
        }

        LOGD("Need decoration for ", ev->view);
        if (decorator_resource)
        {
            auto data = wf::toplevel_cast(ev->view)->toplevel()->get_data_safe<csd_toplevel_custom_data>();

            auto bg = ev->view->get_bounding_box();
            auto vg = wf::toplevel_cast(ev->view)->get_geometry();
            data->margin_offset.x = vg.x - bg.x;
            data->margin_offset.y = vg.y - bg.y;
            LOGD("margin_offsets: ", data->margin_offset.x, ",", data->margin_offset.y);

            if (!should_be_decorated(ev->view))
            {
                return;
            }

            wlr_server_decoration_manager_set_default_mode(
                wf::get_core().protocols.decorator_manager,
                WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
            wf_decorator_manager_send_create_new_decoration(decorator_resource, ev->view->get_id());
        }
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen =
        [=] (wf::view_fullscreen_signal *ev)
    {
        if (ev->view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return;
        }

        auto data = wf::toplevel_cast(ev->view)->toplevel()->get_data<csd_toplevel_custom_data>();

        if (ev->state)
        {
            if (data && data->decoration)
            {
                wf::scene::remove_child(data->decoration->root_node);

                ev->view->damage();
            }
        } else
        {
            if (decorator_resource && data && data->decoration)
            {
                wf::scene::readd_front(ev->view->get_surface_root_node(), data->decoration->root_node);
            }
        }
    };

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [=] (
        wf::txn::new_transaction_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        auto objs = ev->tx->get_objects();
        for (auto& obj : objs)
        {
            if (auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj))
            {
                // First check whether the toplevel already has decoration
                // In that case, we should just set the correct margins
                if (auto deco = toplevel->get_data<csd_toplevel_custom_data>())
                {
                    if (deco && deco->decoration)
                    {
                        toplevel->pending().margins =
                            toplevel->pending().fullscreen ? wf::decoration_margins_t{0, 0, 0,
                            0} : deco_margins;
                        ev->tx->add_object(deco->decoration);
                    } else
                    {
                        toplevel->pending().margins = wf::decoration_margins_t{0, 0, 0, 0};
                    }
                }
            }
        }
    };

    wf::signal::connection_t<wf::txn::object_ready_signal> on_object_ready =
        [=] (wf::txn::object_ready_signal *ev)
    {
        if (!decorator_resource)
        {
            return;
        }

        auto toplvl = dynamic_cast<wf::toplevel_t*>(ev->self);
        auto deco   = toplvl->get_data_safe<csd_toplevel_custom_data>();
        wf::dassert(deco != nullptr, "obj ready for non-decorated toplevel??");
        if (!deco->decoration || !deco->decoration->target_view->get_wlr_surface())
        {
            return;
        }

        if (wlr_xwayland_surface_try_from_wlr_surface(deco->decoration->target_view->get_wlr_surface()))
        {
            deco->decoration->set_final_size(wf::dimensions(toplvl->pending().geometry));
        } else
        {
            deco->decoration->set_final_size(wf::dimensions(toplvl->committed().geometry));
        }
    };

  public:
    void init() override
    {
        decorator_global = wl_global_create(wf::get_core().display,
            &wf_decorator_manager_interface,
            1, NULL, bind_decorator);

        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_pre_map);
        wf::get_core().connect(&on_fullscreen);
        wf::get_core().connect(&on_view_geometry_changed);
        wf::get_core().tx_manager->connect(&on_new_tx);

        auto option_changed = [=] ()
        {
            for (auto& v : wf::get_core().get_all_views())
            {
                if (v->get_app_id() == "org.wf.sample-decorator")
                {
                    continue;
                }

                if (v->role != wf::VIEW_ROLE_TOPLEVEL)
                {
                    continue;
                }

                auto data = wf::toplevel_cast(v)->toplevel()->get_data<csd_toplevel_custom_data>();
                if (data && data->decoration && !should_be_decorated(v))
                {
                    data->decoration->handle_destroy();

                    auto deco_node = data->decoration->deco_node;
                    if (deco_node)
                    {
                        wf::scene::remove_child(deco_node);
                        deco_node.reset();
                    }

                    auto root_node = data->decoration->root_node;
                    if (root_node)
                    {
                        wf::scene::remove_child(root_node);
                        root_node.reset();
                    }

                    data->decoration.reset();

                    wf::get_core().tx_manager->schedule_object(wf::toplevel_cast(v)->toplevel());

                    v->damage();
                }

                data = wf::toplevel_cast(v)->toplevel()->get_data_safe<csd_toplevel_custom_data>();
                if (data && !data->decoration && decorator_resource && should_be_decorated(v))
                {
                    wlr_server_decoration_manager_set_default_mode(
                        wf::get_core().protocols.decorator_manager,
                        WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
                    wf_decorator_manager_send_create_new_decoration(decorator_resource, v->get_id());

                    if ((data->margin_offset.x == -1) && (data->margin_offset.y == -1))
                    {
                        auto bg = v->get_bounding_box();
                        auto vg = wf::toplevel_cast(v)->get_geometry();
                        data->margin_offset.x = vg.x - bg.x;
                        data->margin_offset.y = vg.y - bg.y;
                    }

                    LOGD("margin_offsets: ", data->margin_offset.x, ",", data->margin_offset.y);
                }
            }
        };

        decorate_csd.set_callback(option_changed);
        ignore_views_as_string.set_callback(option_changed);
    }

    void fini() override
    {
        on_mapped.disconnect();
        on_pre_map.disconnect();
        on_fullscreen.disconnect();
        on_view_geometry_changed.disconnect();
        on_new_tx.disconnect();
        destroy_render_instance_manager();
        wl_global_remove(decorator_global);
        if (decorator_client)
        {
            wl_client_flush(decorator_client);
        }

        handle_deco_client_destroy(0, 0);
        wl_global_destroy(decorator_global);
    }
};
}
}

DECLARE_WAYFIRE_PLUGIN(wf::csd_decorator::csd_decoration_plugin);

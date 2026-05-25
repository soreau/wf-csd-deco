/* This plugin is the interface between the decorator client and Wayfire. It has several functions:
 *
 * - When a new view is mapped, it notifies the decorator client via a custom protocol that a new decoration
 *   is required.
 * - When a new decoration toplevel is created, we attach it to the main view with several nodes:
 *   First, we attach the translation node, which has a child mask node, whose child is the decoration surface.
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

#include <wayfire/signal-definitions.hpp>
#include "wf-decorator-protocol.h"

#include <wayfire/unstable/wlr-surface-node.hpp>
#include <wayfire/unstable/wlr-view-events.hpp>
#include <wayfire/unstable/translation-node.hpp>

#define PRIV_COMMIT "_gtk4-deco-priv-commit"

void create_xdg_popup(wlr_xdg_popup *popup);

wf::decoration_margins_t deco_margins =
{
    .left = 0,
    .right = 0,
    .bottom = 0,
    .top = 0,
};

using decoration_node_t = std::shared_ptr<wf::scene::wlr_surface_node_t>;

std::ostream& operator << (std::ostream& out, const wf::dimensions_t& dims)
{
    out << dims.width << "x" << dims.height;
    return out;
}

/**
 * A node which cuts out a part of its children (visually).
 */
class gtk4_mask_node_t : public wf::scene::floating_inner_node_t
{
  public:
    // The 'allowed' portion of the children
    wf::region_t allowed;

    gtk4_mask_node_t() : floating_inner_node_t(false)
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
        instances.push_back(std::make_unique<gtk4_mask_render_instance_t>(this, push_damage, output));
    }

    class gtk4_mask_render_instance_t : public wf::scene::render_instance_t
    {
        std::vector<wf::scene::render_instance_uptr> children;
        wf::scene::damage_callback damage_cb;
        gtk4_mask_node_t *self;

        wf::signal::connection_t<wf::scene::node_damage_signal> on_self_damage =
            [=] (wf::scene::node_damage_signal *ev)
        {
            damage_cb(ev->region);
        };

      public:
        gtk4_mask_render_instance_t(gtk4_mask_node_t *self, wf::scene::damage_callback damage_cb,
            wf::output_t *output)
        {
            this->self = self;
            this->damage_cb = damage_cb;
            for (auto& ch : self->get_children())
            {
                ch->gen_render_instances(children, damage_cb, output);
            }
        }

        void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
            const wf::render_target_t& target, wf::region_t& damage) override
        {
            auto child_damage = damage & self->allowed;
            for (auto& ch : children)
            {
                ch->schedule_instructions(instructions, target, child_damage);
            }
        }

        void compute_visibility(wf::output_t *output, wf::region_t& visible) override
        {
            for (auto& ch : children)
            {
                ch->compute_visibility(output, visible);
            }
        }
    };
};

static const std::string gtk_decorator_prefix = "__wf_decorator:";
std::shared_ptr<wf::scene::translation_node_t> decoration_root_node;
wl_resource *decorator_resource = NULL;
wayfire_toplevel_view target_toplevel_view;
wl_listener deco_client_destroy_listener;
std::vector<std::shared_ptr<wf::scene::wlr_surface_node_t>> deco_nodes;

class gtk4_decoration_object_t : public wf::txn::transaction_object_t
{
    enum class gtk4_decoration_tx_state
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
        out << "gtk4deco(" << this << ")";
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

        LOGI("Final size is ", final, " state is ", (int)deco_state);

        if (this->committed == final)
        {
            switch (this->deco_state)
            {
                case gtk4_decoration_tx_state::STABLE:
                  return;

                case gtk4_decoration_tx_state::START:
                  this->deco_state = gtk4_decoration_tx_state::WAITING_FINAL;
                  break;

                case gtk4_decoration_tx_state::WAITING_FINAL:
                  break;

                case gtk4_decoration_tx_state::TENTATIVE:
                  // fallthrough
                  this->deco_state = gtk4_decoration_tx_state::STABLE;
                  wf::txn::emit_object_ready(this);
                  break;
            }

            return;
        }

        this->committed = final;
        wlr_xdg_toplevel_set_size(toplevel, final.width, final.height);
        this->deco_state = gtk4_decoration_tx_state::WAITING_FINAL;
    }

    void size_updated()
    {
        wlr_box box = toplevel->base->geometry;

        if (wf::dimensions(box) != committed)
        {
            return;
        }
        LOGI("Size is ", wf::dimensions(box), " state is ", (int)deco_state);

        switch (this->deco_state)
        {
            case gtk4_decoration_tx_state::STABLE:
              // Client simply committed, nothing has changed
              return;

            case gtk4_decoration_tx_state::TENTATIVE:
              // Client commits twice?
              return;

            case gtk4_decoration_tx_state::START:
              deco_state = gtk4_decoration_tx_state::TENTATIVE;
              break;

            case gtk4_decoration_tx_state::WAITING_FINAL:
              deco_state = gtk4_decoration_tx_state::STABLE;
              wf::txn::emit_object_ready(this);
              break;
        }
    }

    void commit()
    {
        if (!toplevel)
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        set_pending_size(wf::dimensions(decorated_toplevel->pending().geometry));
        deco_state = gtk4_decoration_tx_state::START;

        LOGI("Committing with ", pending);

        wlr_box box = toplevel->base->geometry;

        if (wf::dimensions(box) != pending)
        {
            wlr_xdg_toplevel_set_size(toplevel, pending.width, pending.height);
        } else
        {
            wf::txn::emit_object_ready(this);
            return;
        }

        committed = pending;
        size_updated();
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

    std::shared_ptr<wf::toplevel_t> decorated_toplevel;

  public:
    gtk4_decoration_object_t(
        wlr_xdg_toplevel *toplevel, wayfire_view target_view, decoration_node_t deco_node,
        std::weak_ptr<gtk4_mask_node_t> mask, std::shared_ptr<wf::toplevel_t> decorated_toplevel,
        std::shared_ptr<wf::scene::translation_node_t> root_node)
    {
        this->toplevel = toplevel;
        this->deco_node = deco_node;
        this->target_view = target_view;
        this->mask_node = mask;
        this->decorated_toplevel = decorated_toplevel;
        this->root_node = root_node;

        on_commit.set_callback([=] (void*)
        {
            pending_state.merge_state(toplevel->base->surface);
            if (deco_state == gtk4_decoration_tx_state::STABLE)
            {
                deco_node->apply_state(std::move(pending_state));
                recompute_mask();
            }

            size_updated();
        });

        on_deco_commit.set_callback([=] (void*)
        {
            auto desired = wf::dimensions(deco_node->get_bounding_box());
            desired.width -= margin_left + margin_right;
            desired.height -= margin_top + margin_bottom + 3;
            if (last_size == desired)
            {
                on_deco_commit.disconnect();
                return;
            }
            auto tg = wf::toplevel_cast(target_view)->get_geometry();
            if (!target_view->get_wlr_surface())
            {
                return;
            }
            if (desired != wf::dimensions(tg))
            {
			    LOGI("Adjusting target on deco commit: ", desired);
                if (wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()))
                {
                    auto vg = wf::toplevel_cast(target_view)->get_geometry();
                    wlr_xwayland_surface_configure(wlr_xwayland_surface_try_from_wlr_surface(target_view->get_wlr_surface()),
                         vg.x, vg.y, desired.width, desired.height);
                } else
                {
                    wlr_xdg_toplevel_set_size(wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface()),
                        desired.width, desired.height);
                }
                last_size = desired;
            }
		});

        on_deco_destroy.set_callback([=] (void*)
        {
            handle_destroy();
            if (decorator_resource)
            {
                target_view->close();
            }
        });

        on_target_destroy.set_callback([=] (void*)
        {
            handle_destroy();
        });

        on_request_move.set_callback([=] (void*)
        {
            wf::get_core().default_wm->move_request(wf::toplevel_cast(target_view));
        });

        on_request_resize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->resize_request(wf::toplevel_cast(target_view));
        });

        on_request_deco_maximize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->tile_request(
                wf::toplevel_cast(target_view),
                wf::toplevel_cast(target_view)->pending_tiled_edges() ?
                0 : wf::TILED_EDGES_ALL);
            handle_maximize();
        });

        on_request_target_maximize.set_callback([=] (void*)
        {
            handle_maximize();
        });

        on_request_minimize.set_callback([=] (void*)
        {
            wf::get_core().default_wm->minimize_request(wf::toplevel_cast(target_view), !wf::toplevel_cast(target_view)->minimized);
        });

        on_new_popup.set_callback([=] (void *data)
        {
            auto popup = (decltype(toplevel->base->popup)) data;

            if (!popup)
            {
                return;
            }
            if (deco_node->get_surface() != popup->parent)
            {
                return;
            }

            popup->parent = target_view->get_wlr_surface();
            create_xdg_popup(popup);
        });

        on_request_move.connect(&toplevel->events.request_move);
        on_request_resize.connect(&toplevel->events.request_resize);
        on_request_deco_maximize.connect(&toplevel->events.request_maximize);
        target_view->get_output()->connect(&on_fullscreen_request);
        target_view->connect(&on_view_title_changed);
        target_view->connect(&on_view_tiled);
        on_request_minimize.connect(&toplevel->events.request_minimize);
        on_new_popup.connect(&wlr_xdg_surface_try_from_wlr_surface(deco_node->get_surface())->client->shell->events.new_popup);
        on_deco_commit.connect(&wlr_xdg_toplevel_try_from_wlr_surface(deco_node->get_surface())->base->surface->events.commit);
        on_commit.connect(&toplevel->base->surface->events.commit);
        on_deco_destroy.connect(&toplevel->events.destroy);
        if (wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface()))
        {
            on_request_target_maximize.connect(&wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface())->events.request_maximize);
            on_target_destroy.connect(&wlr_xdg_toplevel_try_from_wlr_surface(target_view->get_wlr_surface())->events.destroy);
        }

        if (wf::toplevel_cast(target_view)->toplevel()->pending().fullscreen)
        {
            wf::scene::remove_child(root_node);
        } else
        {
            wf::scene::add_front(target_view->get_surface_root_node(), root_node);
        }

        last_size = wf::dimensions(target_view->get_bounding_box());
    }

    void handle_destroy()
    {
        on_commit.disconnect();
        on_deco_commit.disconnect();
        on_deco_destroy.disconnect();
        on_target_destroy.disconnect();
        on_new_popup.disconnect();
        on_request_move.disconnect();
        on_request_resize.disconnect();
        on_request_minimize.disconnect();
        on_request_deco_maximize.disconnect();
        on_request_target_maximize.disconnect();
        on_fullscreen_request.disconnect();
        on_view_title_changed.disconnect();
        on_view_tiled.disconnect();

        this->toplevel = nullptr;

        switch (deco_state)
        {
            case gtk4_decoration_tx_state::STABLE:
              break;
            case gtk4_decoration_tx_state::START:
              // fallthrough
            case gtk4_decoration_tx_state::TENTATIVE:
              // fallthrough
            case gtk4_decoration_tx_state::WAITING_FINAL:
              deco_state = gtk4_decoration_tx_state::STABLE;
              wf::txn::emit_object_ready(this);
              break;
        }
    }

    void handle_maximize()
    {
        if (wf::toplevel_cast(target_view)->pending_tiled_edges() == wf::TILED_EDGES_ALL)
        {
            root_node->set_offset({-margin_left, -margin_top});
		} else if (wf::toplevel_cast(target_view)->pending_tiled_edges())
		{
            root_node->set_offset({use_csd ? -(margin_left - margin_offset.x - margin_offset.x / 2 + 1) : -margin_left, use_csd ? -(margin_top - margin_offset.y - margin_offset.y / 2 - 2) : -margin_top});
        } else
		{
            root_node->set_offset({use_csd ? -(margin_left - margin_offset.x) : -margin_left, use_csd ? -(margin_top - margin_offset.y) : -margin_top});
        }
    }

    wf::signal::connection_t<wf::view_title_changed_signal> on_view_title_changed = [=] (wf::view_title_changed_signal *ev)
    {
        wf_decorator_manager_send_title_changed(decorator_resource, target_view->get_id(), target_view->get_title().c_str());
    };

    wf::signal::connection_t<wf::view_tiled_signal> on_view_tiled = [=] (wf::view_tiled_signal *ev)
    {
        handle_maximize();
    };

    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen_request = [=] (wf::view_fullscreen_signal *ev)
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

    void set_margins(int top, int bottom, int left, int right, wf::point_t offset)
    {
        this->margin_top    = top;
        this->margin_bottom = bottom;
        this->margin_left   = left;
        this->margin_right  = right;
        this->margin_offset  = offset;
    }

    bool use_csd = false;
    wayfire_view target_view;

  private:
    wf::dimensions_t pending = {0, 0};
    wf::dimensions_t committed = {0, 0};

    void recompute_mask()
    {
        auto masked = mask_node.lock();
        wf::dassert(masked != nullptr, "Masked node does not exist anymore??");

        auto bbox = deco_node->get_bounding_box();

        masked->allowed = bbox;
        wf::region_t cut_out = wf::geometry_t {
            .x = bbox.x + margin_left,
            .y = bbox.y + margin_top,
            .width = bbox.width - margin_left - margin_right,
            .height = bbox.height - margin_top - margin_bottom - 3,
        };
        masked->allowed ^= cut_out;
    }

    int margin_left = 0;
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 0;
    wf::point_t margin_offset;

    wf::scene::surface_state_t pending_state;
    std::weak_ptr<gtk4_mask_node_t> mask_node;
    std::shared_ptr<wf::scene::translation_node_t> root_node;

    wlr_xdg_toplevel *toplevel;
    decoration_node_t deco_node;
    wf::dimensions_t last_size;

    wf::wl_listener_wrapper on_commit, on_deco_commit, on_deco_destroy, on_target_destroy, on_new_popup;
    wf::wl_listener_wrapper on_request_move, on_request_resize, on_request_minimize;
    wf::wl_listener_wrapper on_request_deco_maximize, on_request_target_maximize;
    gtk4_decoration_tx_state deco_state = gtk4_decoration_tx_state::STABLE;
};

static bool begins_with(const std::string& a, const std::string& b)
{
    return a.substr(0, b.size()) == b;
}

class gtk4_toplevel_custom_data : public wf::custom_data_t
{
  public:
    std::shared_ptr<gtk4_decoration_object_t> decoration;
};

wf::point_t margin_offset;
nonstd::observer_ptr<gtk4_toplevel_custom_data> deco_custom_data;

void do_update_borders(wl_client*, struct wl_resource*, uint32_t top, uint32_t bottom, uint32_t left, uint32_t right)
{
    deco_margins.top = top - bottom + 2;
    deco_custom_data->decoration->set_margins(top, bottom, left, right, margin_offset);
    bool use_csd = deco_custom_data->decoration->use_csd;
    decoration_root_node->set_offset({use_csd ? -(left - margin_offset.x) : -left, use_csd ? -(top - margin_offset.y) : -top});
    wf::get_core().tx_manager->schedule_object(target_toplevel_view->toplevel());
}

const struct wf_decorator_manager_interface decorator_implementation =
{
    .update_borders = do_update_borders
};

void unbind_decorator(wl_resource*)
{
    LOGI("Unbinding wf-decorator");
    decorator_resource = NULL;
}

static void handle_deco_client_destroy(struct wl_listener *listener, void *data)
{
    unbind_decorator(NULL);
    for (auto &node : deco_nodes)
    {
        wf::scene::remove_child(node);
    }
    for (auto &output : wf::get_core().output_layout->get_outputs())
    {
        output->render->damage_whole();
    }
    deco_nodes.clear();
}

void bind_decorator(wl_client *client, void *, uint32_t, uint32_t id)
{
    LOGI("Binding wf-decorator");
    auto resource = wl_resource_create(client, &wf_decorator_manager_interface, 1, id);

    /* TODO: track active clients */
    wl_resource_set_implementation(resource, &decorator_implementation, NULL, NULL);
    decorator_resource = resource;
    deco_client_destroy_listener.notify = handle_deco_client_destroy;
    wl_client_add_destroy_listener(client, &deco_client_destroy_listener);
}

class gtk4_decoration_plugin : public wf::plugin_interface_t
{
  public:
    wl_global *decorator_global;

    void init_decor(wayfire_view view, wlr_surface *surface)
    {
        LOGI("Got decorator view ", view->get_title());

        auto id_str = std::string(view->get_title()).substr(gtk_decorator_prefix.length());
        auto id = std::stoul(id_str.c_str());

        wayfire_toplevel_view target;
        for (auto& v : wf::get_core().get_all_views())
        {
            if (v->get_id() == id)
            {
                target = toplevel_cast(v);
            }
        }

        target_toplevel_view = target;

        if (!target)
        {
            LOGI("View is gone already?");
            view->close();
            return;
        }

        if (!target->toplevel())
        {
            LOGI("View does not support toplevel interface?");
            view->close();
            return;
        }
        if (!surface)
        {
            LOGI("Premap wlr_surface is null?");
            return;
        }
        if (!wlr_xdg_toplevel_try_from_wlr_surface(surface))
        {
            LOGI("View is not an xdg_toplevel?");
            return;
        }

        deco_custom_data = target->toplevel()->get_data_safe<gtk4_toplevel_custom_data>();

        decoration_root_node = std::make_shared<wf::scene::translation_node_t>();
        auto mask_node = std::make_shared<gtk4_mask_node_t>();
        decoration_root_node->set_children_list({mask_node});

        auto deco_surf = std::make_shared<wf::scene::wlr_surface_node_t>(surface, false);
        deco_nodes.push_back(deco_surf);
        deco_custom_data->decoration = std::make_shared<gtk4_decoration_object_t>(
            wlr_xdg_toplevel_try_from_wlr_surface(surface), target, deco_surf, mask_node, target->toplevel(), decoration_root_node);
        deco_custom_data->decoration->use_csd = !target->should_be_decorated();
        mask_node->set_children_list({deco_surf});

        target->toplevel()->connect(&on_object_ready);
        // Trigger a new transaction to set margins
        wf::get_core().tx_manager->schedule_object(target->toplevel());

        wf_decorator_manager_send_title_changed(decorator_resource, id, target->get_title().c_str());
    };

    wf::signal::connection_t<wf::view_pre_map_signal> on_pre_map = [=] (wf::view_pre_map_signal *ev)
    {
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
        if (ev->view->get_app_id() == "org.wf.sample-decorator")
        {
            return;
        }

        if (ev->view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            LOGI("Not a toplevel");
            return;
        }

        LOGI("Need decoration for ", ev->view);
        if (decorator_resource)
        {
            wlr_server_decoration_manager_set_default_mode(
                wf::get_core().protocols.decorator_manager,
                WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
            wf_decorator_manager_send_create_new_decoration(decorator_resource, ev->view->get_id());
            auto bg = ev->view->get_bounding_box();
            auto vg = wf::toplevel_cast(ev->view)->get_geometry();
            margin_offset.x = vg.x - bg.x;
            margin_offset.y = vg.y - bg.y;
        }
    };

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx = [=] (
        wf::txn::new_transaction_signal *ev)
    {
        auto objs = ev->tx->get_objects();
        for (auto& obj : objs)
        {
            if (auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(obj))
            {
                // First check whether the toplevel already has decoration
                // In that case, we should just set the correct margins
                if (auto deco = toplevel->get_data<gtk4_toplevel_custom_data>())
                {
                    // One thing here: we hardcode the deco_margins. Ideally, these should come from a
                    // protocol
                    toplevel->pending().margins =
                        toplevel->pending().fullscreen ? wf::decoration_margins_t{0, 0, 0, 0} : deco_margins;
                    ev->tx->add_object(deco->decoration);
                }
            }
        }
    };

    wf::signal::connection_t<wf::txn::object_ready_signal> on_object_ready = [=] (wf::txn::object_ready_signal *ev)
    {
        auto toplvl = dynamic_cast<wf::toplevel_t*>(ev->self);
        auto deco = toplvl->get_data_safe<gtk4_toplevel_custom_data>();
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
        wf::get_core().tx_manager->connect(&on_new_tx);
    }
};

DECLARE_WAYFIRE_PLUGIN(gtk4_decoration_plugin);

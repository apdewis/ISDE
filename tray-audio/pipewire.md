# PipeWire Integration Notes

## Volume Control Mechanism

PipeWire has two layers for audio sink volume:

- **Node Props** (`SPA_PARAM_Props` on `pw_node`) — the software-level volume as seen by PipeWire clients. This is what `wpctl set-volume` uses. Setting Props on a node is the correct way to control volume from an external client.

- **Device Route** (`SPA_PARAM_Route` on `pw_device`) — the hardware-level route configuration. Route params map to ALSA mixer controls. Setting a Route param on a device **selects that route as active** (switching the output port), then applies the embedded props. This is intended for route switching (e.g. speakers → headphones), not for volume adjustment.

Use node Props for volume/mute control. Use device Route only for reading hardware state and tracking which route is active.

## Subscribing to Params

After binding a proxy, call `pw_node_subscribe_params` to receive ongoing change notifications. Without subscription, you only get what `pw_node_enum_params` returns (one-shot). The subscription persists for the lifetime of the proxy.

```c
uint32_t params[] = { SPA_PARAM_Props };
pw_node_subscribe_params((struct pw_node *)proxy, params, 1);
pw_node_enum_params((struct pw_node *)proxy, 0, SPA_PARAM_Props, 0, -1, NULL);
```

## Volume Encoding

PipeWire stores `channelVolumes` as linear amplitude (0.0–1.0+). UI tools present volume on a perceptual (cubic) scale so the slider feels linear to the user:

- **UI → PipeWire**: `amplitude = slider_value³`
- **PipeWire → UI**: `slider_value = cbrt(amplitude)`

This matches wpctl and pavucontrol behavior.

## Device Route Params

Device Route events carry `SPA_PARAM_ROUTE_index`, `SPA_PARAM_ROUTE_device`, `SPA_PARAM_ROUTE_direction`, and `SPA_PARAM_ROUTE_props`. The `direction` field distinguishes input/output routes. The `device` field is the card-internal port index (`card.profile.device`), not the PipeWire global device ID.

Sinks may or may not have `card.profile.device` in their node properties. When absent, matching route events to sinks by port index is unreliable — 0 is a valid port index, not a sentinel for "unknown."

## Metadata for Default Sink

The default audio sink is tracked via PipeWire metadata, key `default.audio.sink`, subject 0. The value is JSON: `{"name":"<node.name>"}`. Match against the sink's `PW_KEY_NODE_NAME` property, not its description or ID.

Metadata events may arrive before or after node globals depending on enumeration order. Cache the default sink name so late-arriving sinks can be marked as default during binding.

## Xt Event Loop Integration

PipeWire's loop fd is registered with Xt via `IswAppAddInput`. The input callback calls `pw_loop_enter` / `pw_loop_iterate(loop, 0)` / `pw_loop_leave` to process pending PipeWire events without blocking the Xt main loop.

## Silent Failures

`pw_device_set_param` and `pw_node_set_param` return a sequence number (>0) on message send, not on successful application. The daemon processes the request asynchronously. If the param is malformed or targets a wrong route, the request is silently dropped — no error event, no param event in response. The only way to confirm a change took effect is to check whether the corresponding param subscription delivers an updated value.

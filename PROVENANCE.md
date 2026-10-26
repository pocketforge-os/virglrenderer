# PocketForge fork provenance

- Upstream: https://gitlab.freedesktop.org/virgl/virglrenderer
- Base: `virglrenderer-1.3.0` (`ca50e008863837e094747a69974dde3ae148aeaa`)
- Fork branch: `pocketforge`

PocketForge carries this fork because guest Venus requests dma-buf-exportable
memory while the pinned headless lavapipe host can export only opaque file
descriptors. Upstream merge request
[!632](https://gitlab.freedesktop.org/virgl/virglrenderer/-/merge_requests/632),
“venus: Fallback to opaque fd if dma buf extension unavailable,” was closed
without merging, and the pinned upstream policy in
`src/venus/vkr_device_memory.c` rejects the opaque fallback when dma-buf was
requested.

The fork-local `VCMD_RESOURCE_IMPORT_BLOB` command is test-only and starts at
command 39, after upstream vtest command 38. It models PRIME fd resolution by
attaching the matching server resource to the current test context. Import
policy remains in production Venus renderer code; the test command does not
validate fd type, ownership, allocation size, memory type, or file offset.

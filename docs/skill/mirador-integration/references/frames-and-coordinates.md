# Frames And Coordinates

## Use It For

Submitting capture buffers, rotated or strided input, buffer lifetime, and mapping between frame, oriented, and display coordinates.

## Minimal Usage

A `Frame` wraps a non-owning `ImageView` plus context; `owner` keeps the pixels alive:

```cpp
Frame frame;
frame.image.data = pixels.data();          // const std::byte*
frame.image.width = 320;
frame.image.height = 200;
frame.image.row_stride_bytes = 320 * 4;    // may exceed width * bpp
frame.image.format = PixelFormat::kRgba8;  // kGray8/kRgb8/kBgr8/kRgba8/kBgra8/kNv12
frame.image.rotation = Rotation::k90;      // width/height describe the presented view
frame.image.secondary_plane = ...;         // NV12 chroma only, lives on the view
frame.sequence = 42;
frame.source_id = "screen-1";
frame.owner = std::make_shared<const std::vector<std::byte>>(pixels);
```

Map points, rects, and segments between `CoordinateSpaceId::kFrame` (as captured), `kOriented` (after `rotation`), and `kDisplay` (on screen) with a `Transform2D` you define — for example the window offset as a translation (factories and `compose`/`inverse` live in `transform.hpp`):

```cpp
PointF anchor{100.0F, 50.0F};
RectF box{40.0F, 30.0F, 80.0F, 24.0F};
const Transform2D to_display = make_translation(1920.0, 0.0, CoordinateSpaceId::kOriented,
                                                CoordinateSpaceId::kDisplay);
PointF screen = transform_point(to_display, anchor);
RectF screen_box = transform_rect(to_display, box);
```

Requests declare which space their coordinates live in (`roi_space`, `output_space`, evidence `space`); Mirador recovers results into the space you named.

## Integration Pitfalls

- The view is non-owning. `Frame::owner` must outlive the frame and every result derived from it; copies of a `Frame` share one owner, and once the last reference dies all views into it dangle.
- `width`/`height` are the presented dimensions after `rotation`, not the raw buffer layout; the stride describes the raw rows.
- Set `frame.source_id` equal to the session's `options.source_id`; a mismatch fails with `kInvalidArgument` instead of silently mixing sources.
- NV12 needs a populated `secondary_plane`; single-plane formats must not carry one. `validate(view)` reports the exact violated rule.
- Core never infers the oriented-to-display mapping (window placement is platform knowledge). Capture adapters or your code own that `Transform2D`; fusion demands it when display space is involved.
- Dimensions are bounded by `kMaxImageDimension` (65535); larger views fail validation instead of wrapping.

## Next

Return to the entry router, or read [fusion and snapshots](fusion-and-snapshots.md) when display-space evidence is involved.

import Foundation

/// The compositor's Metal shaders. Kept as a string and
/// compiled at runtime with `MTLDevice.makeLibrary(source:)` so the pod needs
/// no `.metal` build phase; the library is built once per `VideoMixer`.
///
/// Every layer is one textured quad. The vertex shader places the quad from
/// `rect` (NDC: x, top y, width, height) and samples `texRect` (top-left
/// origin, normalized), mirrored horizontally when `mirror` is set (front
/// camera in the preview only). The fragment shader emits premultiplied
/// alpha, matching the pipeline's blend state.
enum MixerShaders {
  static let source = """
  #include <metal_stdlib>
  using namespace metal;

  struct LayerUniforms {
    float4 rect;     // x, top y, width, height in NDC (y up)
    float4 texRect;  // u, v, width, height of the sampled region (top-left origin)
    float mirror;    // 1: flip u (preview of the front camera)
    float3 padding;
  };

  struct VertexOut {
    float4 position [[position]];
    float2 uv;
  };

  vertex VertexOut layer_vertex(uint vid [[vertex_id]], constant LayerUniforms& u [[buffer(0)]]) {
    // A triangle strip: (0,0) (1,0) (0,1) (1,1) in quad space, top-left origin.
    float2 q = float2(float(vid & 1u), float(vid >> 1u));
    VertexOut out;
    out.position = float4(u.rect.x + q.x * u.rect.z, u.rect.y - q.y * u.rect.w, 0.0, 1.0);
    float uCoord = u.mirror > 0.5 ? (1.0 - q.x) : q.x;
    out.uv = float2(u.texRect.x + uCoord * u.texRect.z, u.texRect.y + q.y * u.texRect.w);
    return out;
  }

  fragment float4 layer_fragment(VertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
    return tex.sample(s, in.uv);
  }
  """
}

/// Mirrors `LayerUniforms` in the shader (48 bytes, float4 aligned).
struct LayerUniforms {
  var rect: SIMD4<Float>
  var texRect: SIMD4<Float>
  var mirror: Float
  var padding: (Float, Float, Float) = (0, 0, 0)
}

require "json"

package = JSON.parse(File.read(File.join(__dir__, "package.json")))

Pod::Spec.new do |s|
  s.name         = "NitroRtmp"
  s.version      = package["version"]
  s.summary      = package["description"]
  s.homepage     = package["homepage"]
  s.license      = package["license"]
  s.authors      = package["author"]

  s.platforms    = { :ios => min_ios_version_supported }
  s.source       = { :git => "https://github.com/bhyoo99/react-native-nitro-rtmp.git", :tag => "v#{s.version}" }

  s.source_files = [
    "ios/**/*.{swift}",
    "ios/**/*.{m,mm}",
    # Platform independent core, tested on the host by scripts/test-cpp.sh.
    "cpp/nitrortmp/**/*.{h,hpp,cpp}",
  ]
  # Host-only tests and fixtures live under cpp/__tests__ (also outside the npm package).
  s.exclude_files = ["cpp/__tests__/**"]
  # The C facade is the only core header Swift sees (through the pod's module map).
  # add_nitrogen_files below appends the generated specs to this list.
  s.public_header_files = ["cpp/nitrortmp/nitrortmp_c.h"]

  # Vendored ireader/media-server (MIT): RTMP client, FLV muxer, MPEG-TS muxer.
  # Synced by scripts/sync-media-server.sh, see cpp/third_party/media-server/UPSTREAM.md.
  # A subspec so the flags below apply to these files only: NDEBUG because upstream
  # asserts fire on server input, _IETF_HMAC_ for the digest handshake, -w because
  # upstream is not warning-clean.
  s.subspec "MediaServer" do |ss|
    ss.source_files = "cpp/third_party/media-server/**/*.{h,c}"
    # Keep the vendored C headers out of the module map: generic names such as
    # sha.h or amf0.h must not leak into consumers or clash with other pods.
    ss.private_header_files = "cpp/third_party/media-server/**/*.h"
    ss.compiler_flags = "-DNDEBUG -D_IETF_HMAC_ -w"
  end

  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => [
      "\"$(PODS_TARGET_SRCROOT)/cpp/nitrortmp\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/third_party/media-server/librtmp/include\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/third_party/media-server/libflv/include\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/third_party/media-server/libmpeg/include\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/third_party/media-server/libmov/include\"",
      "\"$(PODS_TARGET_SRCROOT)/cpp/third_party/media-server/sdk/include\"",
    ].join(" "),
  }

  s.dependency 'React-jsi'
  s.dependency 'React-callinvoker'
  # The camera: `CameraLayer.output` is a VisionCamera `CameraOutput` (public Swift + C++ specs).
  s.dependency 'VisionCamera'

  load 'nitrogen/generated/ios/NitroRtmp+autolinking.rb'
  add_nitrogen_files(s)

  install_modules_dependencies(s)
end

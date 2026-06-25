# Mirobody iOS host app

A SwiftUI host app for Mirobody — the iOS counterpart to the Kotlin/Compose app
under [`android/`](../android). Same feature set: email + Google sign-in, streaming
chat, history, settings, font-size and language switching (8 languages), light/dark
theming. It can run as a **pure client** against a remote Mirobody backend, or
**embed** the in-process C++ server (`mirobody.xcframework`) the same way the
Android app loads `libmirobody.so` over JNI.

> Build on macOS. This project is checked in from a Windows machine and has not
> been compiled here — treat the first `xcodegen generate && build` as the
> validation step.

## Prerequisites

- macOS with **Xcode 16+** (iOS 16 deployment target; the project uses Xcode 16
  file-system-synchronized groups).

## Build & run (pure client — no native artifacts needed)

```sh
cd ios
open Mirobody.xcodeproj
```

Then in Xcode: pick a simulator (or your device), set your **Team** under Signing &
Capabilities for device builds, and Run. Swift Package Manager resolves Firebase
and MarkdownUI on first build.

`Mirobody.xcodeproj` is checked in, so no extra tooling is needed. It's also kept
in sync with [`project.yml`](project.yml) — if you prefer to regenerate it (or it
ever drifts), `brew install xcodegen && xcodegen generate` rewrites it from the
spec.

On launch the app talks to the base URL configured in-app (default
`http://localhost:8080`). Set a reachable backend via the gear menu → **Backend**,
e.g. `https://test.mirobody.ai`. Email sign-in works immediately; Google sign-in
needs the one-time Firebase setup below.

### Sideloading onto your own iPhone

With a free Apple ID you can run it on your own device (7-day provisioning):
select your device, set your personal team in Signing, Run. For longer-lived
installs use a paid Apple Developer account.

## Embedding the in-process server (optional)

This makes the app talk to Mirobody **in-process** over loopback instead of a
remote backend — the iOS equivalent of the Android `MirobodyService`/JNI path.

1. Cross-compile `mirobody.xcframework` per the **Building — iOS** section of the
   [root README](../README.md#building--ios) (build the device + simulator slices,
   then `xcodebuild -create-xcframework`).
2. Drop the result at `ios/Frameworks/mirobody.xcframework`.
3. In [`project.yml`](project.yml), uncomment:
   - the `framework: Frameworks/mirobody.xcframework` dependency, and
   - the `SWIFT_ACTIVE_COMPILATION_CONDITIONS: $(inherited) MIROBODY_EMBEDDED` line.
4. (Optional) Add a `config.yml` to the app target to override providers/keys;
   `ServerController` passes it to `mirobody_start`. Without it the bridge uses
   compiled-in defaults.
5. `xcodegen generate` again and rebuild.

`ServerController` (gated by `MIROBODY_EMBEDDED`) calls `mirobody_start` on launch
and the app reaches it at `http://localhost:8080`. The bridge forces loopback, so
no iOS local-network privacy prompt. With the flag off, none of the native symbols
are referenced — the app is a pure client.

## Google sign-in setup (one time; email login needs none of this)

The Android `google-services.json` registers an **Android** app only, so two
iOS-specific Firebase values are unknown and Google sign-in stays hidden until you
supply them. In the [Firebase console](https://console.firebase.google.com)
(project `mirobody-81000`):

1. Add an **iOS app** with bundle id `ai.thetahealth.mirobody`.
2. Open [`Mirobody/Data/Auth/FirebaseInitializer.swift`](Mirobody/Data/Auth/FirebaseInitializer.swift)
   and set:
   - `iosAppID` → the iOS `GOOGLE_APP_ID` (`1:555773021699:ios:…`)
   - `oauthClientID` → the iOS OAuth `CLIENT_ID` (`…apps.googleusercontent.com`)
3. In [`project.yml`](project.yml), replace the `REVERSED_CLIENT_ID` entry under
   `CFBundleURLTypes` with the reversed client id
   (`com.googleusercontent.apps.…`), then regenerate.

Once both values are non-placeholder, `FirebaseInitializer.isConfigured` flips true,
Firebase is configured in code (mirroring the Android `FirebaseInitializer`), and
the "Continue with Google" button appears.

## Apple sign-in setup (native)

Apple sign-in uses the system `ASAuthorizationController` (`AppleAuthRepository`) —
no Firebase, no SDK. The button is always shown on iOS; to make it actually
complete:

1. The "Sign in with Apple" capability is declared in [`project.yml`](project.yml)
   (`entitlements → com.apple.developer.applesignin: [Default]`). Re-run
   `xcodegen generate` after editing the spec.
2. Enable "Sign In with Apple" on the App ID (`ai.thetahealth.mirobody`) in the
   Apple Developer portal, and set a `DEVELOPMENT_TEAM` for device builds.
3. Configure an Apple **Services ID** and set `APPLE_CLIENT_ID` on the backend to
   it — the backend's `AppleTokenValidator` checks the token's `aud` against it.
   (The native app's `aud` is the bundle id; if your backend expects the Services
   ID, register both or set `APPLE_CLIENT_ID` to match the audience your tokens
   actually carry.)

The app POSTs Apple's `identityToken` to `POST /apple/verify`.

## WeChat sign-in setup (OpenSDK)

WeChat sign-in is **compile-gated**: the Tencent SDK isn't a Swift Package, so
`WeChatAuthRepository` and the WeChat button are inert (`isAvailable == false`,
button hidden) until you bundle the SDK. To enable it:

1. Add the WeChat OpenSDK so that `import WechatOpenSDK` resolves — e.g. drop the
   `WechatOpenSDK.xcframework` under `ios/Frameworks/` and add it under the
   target's `dependencies:` in [`project.yml`](project.yml), or add it via
   CocoaPods. The `#if canImport(WechatOpenSDK)` block then activates.
2. Register a **Mobile Application** on the [WeChat Open Platform](https://open.weixin.qq.com)
   with the bundle id + a **Universal Link**; note its AppID/AppSecret.
3. In [`project.yml`](project.yml) set the Info.plist `WechatAppID` /
   `WechatUniversalLink` keys, add the WeChat appid (`wx…`) as a
   `CFBundleURLTypes` scheme, and add the **Associated Domains** entitlement for
   the universal link. `LSApplicationQueriesSchemes` (weixin / weixinULAPI) is
   already present. Regenerate the project.
4. Set `WECHAT_OPEN_APPID` / `WECHAT_OPEN_SECRET` on the backend to the **same**
   Mobile Application credentials (they fall back to `WECHAT_WEB_*` then
   `WECHAT_APPID`).

The app sends a `SendAuthReq` (scope `snsapi_userinfo`); WeChat returns via the
universal link (handled in `MirobodyApp.onOpenURL` → `WXApi.handleOpen`), and the
OAuth code is exchanged via `POST /wechat/verify` with `flow=app`.

## Health data (Apple HealthKit)

The app reads on-device Apple Health data and pushes it to the backend's FHIR
endpoint — the iOS side of the on-device ingestion path (HealthKit is on-device
only; there is no Apple cloud API). See [src/health/README.md](../src/health/README.md).

- **Reader** (`Data/Health/HealthKitRepository.swift`): requests read access to
  steps / heart rate / sleep / weight, reads the last 7 days, maps each to a FHIR
  R4 `Observation` (`FhirObservation`), and `POST`s to `/fhir/Observation` via
  `ApiClient.postRaw`.
- **UI**: settings gear → **Sync health data** (`UI/Health/HealthSyncView`), which
  requests authorization then runs the sync.
- **Setup**: the HealthKit capability and `NSHealthShareUsageDescription` are
  declared in [`project.yml`](project.yml)
  (`entitlements → com.apple.developer.healthkit`). Re-run `xcodegen generate`
  after editing, enable **HealthKit** on the App ID in the Apple Developer portal,
  and set a `DEVELOPMENT_TEAM` for device builds. It's read-only, so no
  `NSHealthUpdateUsageDescription` is needed.
- Labels in `HealthSyncView` are English literals for now — localize via the
  `.lproj` tables (the `L()` helper returns the raw key when a locale lacks it, so
  there's no automatic en fallback).

## Project layout

```
ios/
  Mirobody.xcodeproj          # committed Xcode project (open this)
  project.yml                 # XcodeGen spec — regenerates the project if needed
  Mirobody/
    App/                      # @main app, DI container, embedded-server controller
    Bridging/                 # Objective-C bridging header → src/mirobody.h
    Data/
      Net/                    # ApiClient (URLSession), envelope, error taxonomy, JSONValue
      Auth/                   # DTOs, AuthRepository, Firebase Google sign-in
      Chat/                   # DTOs, SSE stream client, ChatRepository
      Config/                 # /mirobody.json model + store
      Health/                 # HealthKitRepository, FHIR mapper (Apple Health → /fhir)
      Settings/               # UserDefaults-backed settings
    UI/
      RootView.swift          # token-driven navigation + error toast
      Theme.swift             # colors + type scale + font scaling
      Localization.swift      # runtime language override (LText / L())
      Auth/ Chat/ Health/ Settings/   # screens + view models
    Resources/
      *.lproj/Localizable.strings   # en, zh, ja, ko, fr, de, ru, es
      Assets.xcassets
```

### How it maps to the Android app

| Android (Kotlin/Compose)        | iOS (SwiftUI)                                  |
| ------------------------------- | ---------------------------------------------- |
| `MirobodyService` + JNI         | `ServerController` + `src/mirobody.h` C API    |
| Retrofit + OkHttp interceptors  | `ApiClient` (URLSession)                       |
| OkHttp SSE `EventSource`        | `ChatStreamClient` (`URLSession.bytes`)        |
| DataStore                       | `SettingsStore` (UserDefaults)                 |
| Navigation Compose graph        | `RootView` (token-driven) + `NavigationStack`  |
| `ProvideLocale` runtime locale  | `mbLanguage` environment + `LText`/`L()`       |
| Markwon markdown                | MarkdownUI                                      |
| Coil `AsyncImage` (+ SVG)       | `AsyncImage` + `SVGWebView` (`WKWebView`)      |
| Firebase Auth (OAuth web flow)  | FirebaseAuth (`OAuthProvider("google.com")`)   |
| Health Connect / HMS Health Kit | HealthKit (`HealthKitRepository`)              |

## Known gaps vs. Android

- **LaTeX math** in chat (`$…$` / `$$…$$`) is not rendered — Markwon's JLatexMath
  plugin has no drop-in SwiftUI equivalent. Other markdown (tables, code,
  strikethrough, links) works via MarkdownUI.
- History is presented as a **sheet** rather than a left navigation drawer (the
  iOS-idiomatic equivalent); behavior (view/delete, refresh-on-open) matches.

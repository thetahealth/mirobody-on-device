<div align="center">

# mirobody-on-device

**mirobody 的手机端运行时：你的健康记录，以及回答你问题的模型，都留在手机上。**

**[English](README.md)** · **中文**

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)
[![C++11](https://img.shields.io/badge/C%2B%2B-11-00599C.svg?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CI](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml/badge.svg)](https://github.com/thetahealth/mirobody-on-device/actions/workflows/ci.yml)
[![Platforms](https://img.shields.io/badge/platforms-Android%20·%20iOS%20·%20HarmonyOS-lightgrey.svg)](#构建应用)

**[mirobody 服务端](https://github.com/thetahealth/mirobody)** · **[📚 文档](https://docs.mirobody.ai/)** · **[mirobody-web](https://github.com/thetahealth/mirobody-web)**

</div>

---

手表把步数和心率同步到手机，戒指写入睡眠，血压计写入血压。你的健康数据，手机上存得比任何服务器都全，
而且手机是唯一一个这些数据根本不必离开的地方。mirobody-on-device 是一个跑在 Android、iOS、鸿蒙 App
*内部* 的 C++ 核心：它读取只有手机才拿得到的数据，把每条读数编码到 LOINC 和 UCUM，存进本地 SQLite
记录，再由手机上运行的模型在这份记录上回答你的问题。把 App 指向一台
[mirobody](https://github.com/thetahealth/mirobody) 服务器，它就变成那台服务器的客户端。

<p align="center">
  <img src="docs/images/what-is-mirobody.svg" alt="What is Mirobody? One health AI that runs anywhere and keeps your data yours: on a server (self-hosted, the whole family), on your phone (just you, works offline), or peer-to-peer." width="920">
</p>

## 它做什么

- **采集只有手机才拿得到的数据。** Apple HealthKit、Android Health Connect、华为运动健康，以及 Android
  和 iOS 上的标准蓝牙健康设备（GATT / IEEE-11073）。
- **按服务端的方式编码。** 每条读数都有表示测了什么的 LOINC 编码和 UCUM 单位，存为 FHIR
  `Observation`，原始上传文件原样保留。编码表目前在各个 App 里，正在迁移到主仓发布的设备词表，
  让手机和服务器对同一个字段的含义不可能有分歧。
- **离线回答。** 本地 agent 循环在本地记录上调用同一套 MCP 风格的工具，由手机上的模型驱动：
  LiteRT-LM 或 llama.cpp，Gemma 级别，1–4B 参数、4-bit 量化。
- **或者用你自己的 key，直连模型厂商。** 自带 key（BYOK）时模型在云端，记录仍在手机上。
- **一套 C ABI，三个 App。** App 通过 [`src/mirobody.h`](src/mirobody.h) 里的 24 个函数驱动核心，
  不存在每个平台各写一份的实现。

<p align="center">
  <img src="docs/images/where-your-data-comes-from.svg" alt="Where your data comes from: wearables, phone health, lab results, clinic records, and everyday photo or voice logging all flow into mirobody, which normalizes everything to FHIR R4, then a model answers in plain language." width="920">
</p>

## 两分钟试一试

核心可以在 Linux 或 macOS 桌面上构建运行，不需要手机：

```sh
# macOS（Linux 的 apt 命令见 docs/BUILDING.md）
brew install cmake ninja pkg-config libwebsockets openssl@3 rapidjson yaml-cpp \
             sqlite jpeg-turbo libpng libtiff webp catch2

git clone https://github.com/thetahealth/mirobody-on-device.git && cd mirobody-on-device
./build.sh                    # 核心、本机回环服务、CLI、测试 -> build/
build/tests/mirobody_tests    # 单元测试
build/fhir normalize "5.62 mmol/L" "72 bpm"
```

```json
{"input":"5.62 mmol/L","comparator":"","value":5.62,"unit":"mmol/L","family":"SCnc"}
{"input":"72 bpm","comparator":"","value":72.0,"unit":"/min","family":"NRat"}
```

`bpm` 不是 UCUM 单位，出来的是 `/min`，外加 LOINC 属性族（`NRat`，数量速率），它决定这条读数能归到
哪些编码下。`build/mcp list` 列出手机端 agent 能调用的工具。`build/mirobody` 启动 App 所连的本机回环
服务：它读取 [`config.example.yml`](config.example.yml)，只监听 `127.0.0.1:8080`，
`curl 127.0.0.1:8080/api/health` 返回 `ok`。

## 与 mirobody 的分工

| 职责 | 所在 |
|---|---|
| 服务端：账号、互助圈、Postgres、Garmin / Oura / Whoop 拉取、文件抽取、agent、MCP、导出 / 导入 | [mirobody](https://github.com/thetahealth/mirobody) |
| 术语（LOINC / UCUM / ICPC-3 / 设备对照表），唯一的真相来源 | mirobody，以数据形式发布，本仓消费 |
| API 契约、SSE 传输格式、`/mirobody.json` 能力声明 | mirobody（[docs.mirobody.ai](https://docs.mirobody.ai/)） |
| Web 界面 | [mirobody-web](https://github.com/thetahealth/mirobody-web) |
| 桌面本地部署 | mirobody（Docker，以及本地模型配置） |
| **手机 App、原生采集、离线核心、C ABI** | **本仓** |

服务器能做的功能，属于主仓。留在这里的，是离不开手机的部分：它的健康数据库、它的传感器、它的离线模型，
以及它的沙箱。

## 隐私

两个相互独立的选择，默认都在设备上：

| 通道 | 模型 | 记录 |
|---|---|---|
| **端侧** | 在手机上运行 | 在手机上，什么都不离开 |
| **BYOK** | 用户自己的 key，直连模型厂商 | 在手机上；这一轮对话（以及其中的健康上下文）会发给厂商 |
| **mirobody 服务器** | 该服务器配置的模型 | 在那台服务器上（自托管或托管） |

本机回环服务只绑定 `127.0.0.1`，即使调用方要求 `0.0.0.0` 也一样：核心里存着健康记录，不该暴露在网络上。
完整矩阵、每类数据离开设备的去向，以及如实的注意事项，见 [docs/privacy-tiers.md](docs/privacy-tiers.md)。
在依赖其中任何一条之前，请先读 [SECURITY.md](SECURITY.md)。

mirobody 帮你整理、解释你自己的健康数据。它不做诊断，不开处方，也不替代医生。

<p align="center">
  <img src="docs/images/on-device-llm.svg" alt="On-device LLM: one private chat model on the hardware you already own, with no server round-trip, quantized to fit device memory, GPU-accelerated, and swappable." width="920">
</p>

## 构建应用

| App | 如何链接核心 | 指南 |
|---|---|---|
| Android | 通过 JNI 加载 `libmirobody.so` | [android/](android/README.md) |
| iOS | `mirobody.xcframework`，静态库 | [ios/](ios/README.md) |
| 鸿蒙 | NAPI，构建时不含 HTTP 服务（`MIROBODY_MOBILE`） | [harmony/](harmony/README.md) |

每个 App 需要先交叉编译一次依赖（预编译 sysroot）；[docs/BUILDING.md](docs/BUILDING.md) 覆盖所有目标，
包括 Windows（`build.cmd` 配合 vcpkg）。`./build.sh mobile` 在桌面上构建鸿蒙配置，这是桌面上最接近手机构建
的方式：该配置下核心静态库为 3.8 MiB，开发配置为 5.4 MiB（macOS arm64）。端侧模型的运行时、格式、量化和
手机实测数据，见 [docs/on-device-llm.md](docs/on-device-llm.md)。

## 目录结构

```
src/                  C++ 核心（C++11）
  mirobody.h          公开的 C ABI：嵌入接口
  platform/           JNI（Android）与 iOS 桥接，C ABI 的实现
  chat/  llm/  mcp/   agent 循环、流式模型客户端、工具注册表
  fhir/  indicator/   FHIR 存储与写入路径、术语解析
  health/             端侧写入（健康数据批次 -> FHIR）
  database/ storage/  SQLite 与本地文件存储
  server/             本机回环 HTTP 服务（Android / iOS 目前在用；
                      鸿蒙配置构建时不含它）
res/                  agent、MCP 工具、SQLite schema、术语数据
android/ ios/ harmony/  三个宿主 App
tests/                C++ 单元测试
cli/                  各子系统的调试工具
tools/                仓库维护脚本
docs/                 构建指南、隐私分级、端侧模型、Markdown 规范
```

## 接下来

本仓原是早期 mirobody 服务端的完整 C++ 移植（"mirobody v2"）。2026 年 9 月起收窄到手机端，并分步与主仓对齐：

1. **精简**只有服务器才需要的部分。桌面与 Web 客户端、服务端数据库与对象存储、Redis、云厂商接入、
   托管记忆服务和实时语音通道已经删除。账号、互助圈和 OAuth 层是下一步，与 App 切换到每次启动生成的一次性令牌
   同时进行；之后 HTTP 服务改为可选模块。
2. **一套契约。** 完全使用主仓的 API 和 SSE 传输格式，响应同一份能力声明，并把 mirobody-web 的构建产物放进
   App 的 WebView，让手机和服务器共用一套界面。
3. **一套词表。** 加载主仓发布的术语，不再单独构建词库。
4. **一种记录格式。** 与 mirobody 服务器同步，并导出 / 导入同样的 FHIR Bundle。

一路删掉的内容都保存在
[`v2-full-2026-08`](https://github.com/thetahealth/mirobody-on-device/tree/v2-full-2026-08) tag。
每次变更的内容见 [CHANGELOG](CHANGELOG.md)。

## 🤝 参与贡献

最有价值的反馈，是一条被手机编错的读数：HealthKit、Health Connect 或华为运动健康的某个字段，被归到了错误的编码
或单位下，或者没有编码。请用健康数据库里的原始字段名
[提交问题](https://github.com/thetahealth/mirobody-on-device/issues/new?template=wrong-reading.yml)。
Pull request 必须通过的检查与 CI 相同：

```sh
./build.sh && build/tests/mirobody_tests && ./build.sh mobile
python3 tools/check_doc_links.py && python3 tools/check_exports.py
```

→ [CONTRIBUTING.md](CONTRIBUTING.md) · [AGENTS.md](AGENTS.md)（写给编码 agent）·
[SECURITY.md](SECURITY.md) · [行为准则](CODE_OF_CONDUCT.md) · [CHANGELOG](CHANGELOG.md)

## 📚 文档

[`docs/`](docs/README.md) 包含构建指南、隐私分级、端侧模型演示稿，以及所有客户端都要遵循的渲染规范；
`src/` 下的子系统大多有各自的 README。服务端、API 和术语的文档在 **[docs.mirobody.ai](https://docs.mirobody.ai/)**。

<div align="center">

Apache 2.0 · © 2026 [Theta Health](https://thetahealth.ai)

</div>

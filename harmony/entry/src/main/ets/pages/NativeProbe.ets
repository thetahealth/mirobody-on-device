/**
 * Dev probe for the embedded C++ core (temporary; remove once the native
 * transport has proven out on devices).
 *
 * Buttons, in intended order:
 *   1. init      — inject stored BYOK keys into the core and list what it can
 *                  run. Empty list + no keys saved is the expected cold state;
 *                  paste a key in the provider manager first.
 *   2. chat      — one real turn ("hi") through the first listed provider,
 *                  events streaming into the list below with arrival offsets.
 *                  Cancel mid-stream with the same button to verify abort.
 */
import mirobody from 'libmirobody.so';
import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import fileUri from '@ohos.file.fileuri';
import fileShare from '@ohos.fileshare';
import Environment from '@ohos.file.environment';
import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';
import { BusinessError } from '@kit.BasicServicesKit';
import { NativeCore } from '../core/NativeCore';
import { LocalModelStore, LocalModel, DownloadDir, LOCAL_PROVIDER_ID } from '../core/LocalModelStore';
import { ConfigStore, ActiveSelection } from '../core/ConfigStore';
import { ChatMessage } from '../model/Chat';
import { FONT } from '../model/Typography';

interface ProbeLine {
  type: string;
  content: string;
  atMs: number;
}

@Entry
@Component
struct NativeProbe {
  @State status: string = '(not initialized)';
  @State providers: string[] = [];
  @State lines: ProbeLine[] = [];
  @State running: boolean = false;

  private startedAt: number = 0;
  private cancelTurn: (() => void) | undefined = undefined;

  private async init(): Promise<void> {
    try {
      this.status = mirobody.nativeVersion();
      await NativeCore.ensureReady(getContext(this) as common.UIAbilityContext);
      const listed: string = mirobody.nativeGetProviders();
      this.providers = listed.length > 0 ? listed.split('\n') : [];
      this.status += this.providers.length > 0
        ? ` | ${this.providers.length} provider(s)`
        : ' | no providers (no keys injected — save one in the provider manager)';
    } catch (e) {
      this.status = 'init FAILED: ' + JSON.stringify(e);
    }
  }

  private chat(): void {
    if (this.running) {
      if (this.cancelTurn !== undefined) {
        this.cancelTurn();
      }
      return;
    }
    if (this.providers.length === 0) {
      this.status = 'no provider to chat with — run init first';
      return;
    }
    this.running = true;
    this.lines = [];
    this.startedAt = new Date().getTime();

    const messages: ChatMessage[] = [{ role: 'user', content: '用一句话介绍你自己' }];
    const turnId: number = mirobody.nativeChat(this.providers[0], JSON.stringify(messages),
      (type: string, content: string) => {
        this.lines = this.lines.concat({
          type: type,
          content: content,
          atMs: new Date().getTime() - this.startedAt
        } as ProbeLine);
        if (type === 'end' || type === 'aborted') {
          this.running = false;
          this.cancelTurn = undefined;
        }
      });
    this.cancelTurn = () => {
      mirobody.nativeChatCancel(turnId);
    };
  }

  private log(type: string, content: string): void {
    this.lines = this.lines.concat({
      type: type, content: content, atMs: new Date().getTime() - this.startedAt
    } as ProbeLine);
    // Mirrored to hilog so a run can be read off the wire instead of the screen.
    hilog.info(0x3200, 'mirobody-probe', '%{public}s | %{public}s', type, content);
  }

  /**
   * Decides where a GGUF is allowed to live.
   *
   * A model must survive app uninstall (a multi-GB download is too expensive to
   * lose), so it has to sit outside app-private storage — the same conclusion the
   * Android client reached. But llama.cpp loads by PATH and mmaps: an ArkTS-
   * readable URI or fd is not enough. A @ohos.fileshare grant is URI-scoped, so
   * whether it also makes the underlying path openable by native code is unknown
   * and cannot be settled by reading docs. This reports, per candidate, what
   * ArkTS sees and what native sees.
   */
  /**
   * Is the NPU reachable at all? Answered before spending anything on model
   * conversion, because NNRt is the only route to it — MindSpore Lite delegates
   * through NNRt, and HMS `hiai_foundation`/`CANNKit` expose single-op execution
   * with no model loading. A device list with no `type: 3` (ACCELERATOR) entry
   * means the NPU is not exposed to third-party apps and the path is closed
   * whatever the op coverage turns out to be.
   */
  private nnrtDevices(): void {
    this.lines = [];
    this.startedAt = new Date().getTime();
    const json: string = mirobody.nativeNnrtDevices();
    this.log('nnrt', json);
    // Spelled out rather than left to the reader: `type: 3` is the whole point.
    this.log('verdict', json.indexOf('"type":3') >= 0
      ? 'an ACCELERATOR is exposed — op coverage is now the question'
      : 'no ACCELERATOR — NPU not reachable from a third-party app');

    // The ceiling no accelerator can lift. Reported next to the NNRt answer
    // because on its own "the NPU is reachable" reads as encouragement.
    const bw: string = mirobody.nativeMemBandwidth();
    this.log('bandwidth', bw);
    const m = bw.match(/"allThreadGbs":([0-9.]+)/);
    if (m !== null && m.length > 1) {
      const gbs = Number.parseFloat(m[1]);
      // Qwen3.5-2B-Q4_K_M at the measured 11.2 tok/s: 1.28 GB read per token.
      this.log('ceiling', 'decode ceiling for a 1.28 GB model = '
        + (gbs / 1.28).toFixed(1) + ' tok/s; measured 11.2 tok/s');
    }
  }

  private probePaths(): void {
    this.lines = [];
    this.startedAt = new Date().getTime();
    this.status = mirobody.nativeLocalStatus();

    let downloadDir = '';
    try {
      downloadDir = Environment.getUserDownloadDir();
      this.log('downloadDir', downloadDir);
    } catch (e) {
      const err = e as BusinessError;
      this.log('downloadDir', 'FAILED code=' + err.code + ' ' + err.message);
    }

    let documentDir = '';
    try {
      documentDir = Environment.getUserDocumentDir();
      this.log('documentDir', documentDir);
    } catch (e) {
      const err = e as BusinessError;
      this.log('documentDir', 'FAILED code=' + err.code + ' ' + err.message);
    }

    const ctx = getContext(this) as common.UIAbilityContext;

    // Positive control: a file we KNOW exists, written through ArkTS. Without it a
    // run of all-ENOENT can't distinguish "path unreachable" from "probe broken".
    const control = ctx.filesDir + '/probe-control.bin';
    try {
      const f = fs.openSync(control, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, 'control');
      fs.closeSync(f);
      this.log('control', 'wrote ' + control);
    } catch (e) {
      const err = e as BusinessError;
      this.log('control', 'write FAILED code=' + err.code + ' ' + err.message);
    }

    // Only in-sandbox candidates: the earlier run proved the app's mount namespace
    // has no /data/local/tmp and no /storage/media/100/... (ENOENT, not EACCES),
    // so shell-visible paths are not a route at all.
    const candidates: string[] = [
      control,
      // No grant of any kind exists for this one. Measured result: EPERM — so the
      // public directory is NOT readable outright, access really is grant-gated,
      // and the app cannot wander outside what the user handed it. Keep it as the
      // regression test for that property.
      '/storage/Users/currentUser/never-picked.bin'
    ];
    if (downloadDir.length > 0) { candidates.push(downloadDir + '/model.gguf'); }
    if (documentDir.length > 0) { candidates.push(documentDir + '/model.gguf'); }
    for (const p of candidates) {
      this.log('native', p + ' -> ' + mirobody.nativeProbePath(p));
    }
  }

  /**
   * The local model the composer currently has selected — read from ConfigStore, the
   * app's single active-selection store, so this page can never disagree with the
   * main UI about which model is active.
   */
  private async activeLocal(): Promise<LocalModel | undefined> {
    const ctx = getContext(this) as common.UIAbilityContext;
    const sel: ActiveSelection = await ConfigStore.load(ctx);
    if (sel.providerId !== LOCAL_PROVIDER_ID) {
      return undefined;
    }
    return await LocalModelStore.byId(ctx, sel.model);
  }

  /** Import a .gguf through the store, then verify native can really load it. */
  private async importModel(): Promise<void> {
    this.lines = [];
    this.startedAt = new Date().getTime();
    const ctx = getContext(this) as common.UIAbilityContext;

    const m: LocalModel | undefined = await LocalModelStore.importModel(ctx);
    if (m === undefined) {
      this.log('import', 'cancelled or unreachable (see mirobody-models log)');
      return;
    }
    // Written to ConfigStore, exactly as the composer's picker does.
    await ConfigStore.save(ctx, { providerId: LOCAL_PROVIDER_ID, model: m.id });
    this.log('imported', m.displayName + '  ' + m.sizeBytes + ' bytes');
    this.log('path', m.path);
    // The store already stat'd it from ArkTS; this is the native verdict, which is
    // the one that matters because the engine mmaps rather than reads.
    this.log('native', mirobody.nativeProbePath(m.path));
  }

  /** Let the user name a public download directory (Environment.* is 801 here). */
  private async pickDir(): Promise<void> {
    this.lines = [];
    this.startedAt = new Date().getTime();
    const ctx = getContext(this) as common.UIAbilityContext;
    const dir: DownloadDir | undefined = await LocalModelStore.pickDownloadDir(ctx);
    if (dir === undefined) {
      this.log('dir', 'cancelled or failed');
      return;
    }
    this.log('dir', dir.path);
    this.log('native', mirobody.nativeProbePath(dir.path));
  }

  /**
   * Sweep thread counts on the same prompt and model. One tap instead of a
   * rebuild-and-retap per value: the count is a runtime knob, and the model stays
   * resident across turns so only the first pass pays the load.
   *
   * Runs sequentially and awaits each turn — concurrent turns would contend for the
   * same cores and measure nothing.
   */
  private async threadScan(): Promise<void> {
    const m: LocalModel | undefined = await this.activeLocal();
    if (m === undefined) {
      this.lines = [];
      this.log('scan', 'no local model selected — pick one in the composer first');
      return;
    }
    this.lines = [];
    this.startedAt = new Date().getTime();
    this.running = true;

    // 0 = whatever hardware_concurrency() reports, i.e. today's default.
    const counts: number[] = [0, 2, 4, 6, 8];
    const messages: ChatMessage[] = [{ role: 'user', content: '用一句话介绍你自己' }];

    for (const n of counts) {
      mirobody.nativeLocalSetThreads(n);
      this.log('scan', 'threads=' + (n === 0 ? 'auto' : n.toString()));
      await new Promise<void>((resolve) => {
        mirobody.nativeLocalChat(m.path, JSON.stringify(messages),
          (type: string, content: string) => {
            // Only the numbers matter here; the reply text is the same every pass.
            if (type === 'stats' || type === 'error') {
              this.log(type, content);
            }
            if (type === 'end' || type === 'aborted') {
              resolve();
            }
          });
      });
    }
    // Back to the default so the scan doesn't silently pin a value for later turns.
    mirobody.nativeLocalSetThreads(0);
    this.running = false;
    this.log('scan', 'done (threads reset to auto)');
  }

  /** One real on-device turn against the active model. */
  private localChat(): void {
    if (this.running) {
      if (this.cancelTurn !== undefined) { this.cancelTurn(); }
      return;
    }
    this.lines = [];
    this.startedAt = new Date().getTime();

    this.activeLocal().then((m: LocalModel | undefined) => {
      if (m === undefined) {
        this.log('local', 'no local model selected — pick one in the composer first');
        return;
      }
      this.running = true;
      this.log('model', m.displayName + ' @ ' + m.path);
      const messages: ChatMessage[] = [{ role: 'user', content: '用一句话介绍你自己' }];
      const turnId: number = mirobody.nativeLocalChat(m.path, JSON.stringify(messages),
        (type: string, content: string) => {
          this.log(type, content);
          if (type === 'end' || type === 'aborted') {
            this.running = false;
            this.cancelTurn = undefined;
          }
        });
      this.cancelTurn = () => { mirobody.nativeChatCancel(turnId); };
    });
  }

  build() {
    Column() {
      Text('Native core probe')
        .fontSize(FONT.headline)
        .fontWeight(FontWeight.Bold)
        .width('100%')
        .padding({ left: 20, right: 20, top: 16, bottom: 16 })
        .border({ width: { bottom: 0.5 }, color: $r('app.color.border') })

      Text(this.status)
        .fontSize(FONT.label)
        .fontColor(this.status.indexOf('FAILED') >= 0 ? $r('app.color.danger') : $r('app.color.text_secondary'))
        .width('100%')
        .padding({ left: 20, right: 20, top: 12, bottom: 12 })

      Row({ space: 12 }) {
        Button('init + providers')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .layoutWeight(1)
          .onClick(() => this.init())
        Button(this.running ? 'cancel' : 'chat')
          .fontSize(FONT.body)
          .backgroundColor(this.running ? $r('app.color.danger') : $r('app.color.success'))
          .layoutWeight(1)
          .onClick(() => this.chat())
      }
      .width('100%')
      .padding({ left: 20, right: 20, bottom: 12 })

      // Where may a GGUF live? See probePaths() — this is the gate on the whole
      // "model outside app storage" design.
      Row({ space: 12 }) {
        Button('probe paths')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .layoutWeight(1)
          .onClick(() => this.probePaths())
        Button('import .gguf')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .layoutWeight(1)
          .onClick(() => this.importModel())
        Button('nnrt devices')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .layoutWeight(1)
          .onClick(() => this.nnrtDevices())
      }
      .width('100%')
      .padding({ left: 20, right: 20, bottom: 12 })

      Row({ space: 12 }) {
        Button('pick download dir')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .layoutWeight(1)
          .onClick(() => this.pickDir())
        Button(this.running ? 'cancel' : 'local chat')
          .fontSize(FONT.body)
          .backgroundColor(this.running ? $r('app.color.danger') : $r('app.color.success'))
          .layoutWeight(1)
          .onClick(() => this.localChat())
        Button('thread scan')
          .fontSize(FONT.body)
          .backgroundColor($r('app.color.brand'))
          .enabled(!this.running)
          .layoutWeight(1)
          .onClick(() => this.threadScan())
      }
      .width('100%')
      .padding({ left: 20, right: 20, bottom: 12 })

      if (this.providers.length > 0) {
        Text(this.providers.join('  ·  '))
          .fontSize(FONT.caption)
          .fontColor($r('app.color.brand_text'))
          .width('100%')
          .padding({ left: 20, right: 20, bottom: 8 })
      }

      List({ space: 6 }) {
        ForEach(this.lines, (line: ProbeLine, index: number) => {
          ListItem() {
            Row({ space: 10 }) {
              Text('+' + line.atMs + 'ms')
                .fontSize(FONT.caption)
                .fontColor($r('app.color.text_tertiary'))
                .width(76)
              Text(line.type)
                .fontSize(FONT.caption)
                .fontColor(line.type === 'end' ? $r('app.color.success')
                  : (line.type === 'error' || line.type === 'aborted') ? $r('app.color.danger_text') : $r('app.color.brand_text'))
                .width(92)
              Text(line.content)
                .fontSize(FONT.label)
                .fontColor($r('app.color.text_primary'))
                .layoutWeight(1)
            }
            .width('100%')
            .padding({ left: 20, right: 20, top: 6, bottom: 6 })
          }
        }, (line: ProbeLine, index: number) => line.type + '-' + index)
      }
      .layoutWeight(1)
      .width('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.bg_page'))
  }
}

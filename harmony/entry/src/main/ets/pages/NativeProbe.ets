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
import { NativeCore } from '../core/NativeCore';
import { ChatMessage } from '../model/Chat';

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
      const listed = mirobody.nativeGetProviders();
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
    const turnId = mirobody.nativeChat(this.providers[0], JSON.stringify(messages),
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

  build() {
    Column() {
      Text('Native core probe')
        .fontSize(20)
        .fontWeight(FontWeight.Bold)
        .width('100%')
        .padding({ left: 20, right: 20, top: 16, bottom: 16 })
        .border({ width: { bottom: 0.5 }, color: '#E5E5E5' })

      Text(this.status)
        .fontSize(13)
        .fontColor(this.status.indexOf('FAILED') >= 0 ? '#D24C57' : '#666666')
        .width('100%')
        .padding({ left: 20, right: 20, top: 12, bottom: 12 })

      Row({ space: 12 }) {
        Button('init + providers')
          .fontSize(14)
          .backgroundColor('#3F7FC1')
          .layoutWeight(1)
          .onClick(() => this.init())
        Button(this.running ? 'cancel' : 'chat')
          .fontSize(14)
          .backgroundColor(this.running ? '#D24C57' : '#46A86A')
          .layoutWeight(1)
          .onClick(() => this.chat())
      }
      .width('100%')
      .padding({ left: 20, right: 20, bottom: 12 })

      if (this.providers.length > 0) {
        Text(this.providers.join('  ·  '))
          .fontSize(12)
          .fontColor('#3F7FC1')
          .width('100%')
          .padding({ left: 20, right: 20, bottom: 8 })
      }

      List({ space: 6 }) {
        ForEach(this.lines, (line: ProbeLine, index: number) => {
          ListItem() {
            Row({ space: 10 }) {
              Text('+' + line.atMs + 'ms')
                .fontSize(12)
                .fontColor('#9AA0A6')
                .width(76)
              Text(line.type)
                .fontSize(12)
                .fontColor(line.type === 'end' ? '#46A86A'
                  : (line.type === 'error' || line.type === 'aborted') ? '#D24C57' : '#3F7FC1')
                .width(92)
              Text(line.content)
                .fontSize(13)
                .fontColor('#1A1A1A')
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
    .backgroundColor(Color.White)
  }
}

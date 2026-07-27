/**
 * Provider / key manager — the BYOK analogue of Android's on-device model dialog.
 *
 * Corrected mental model: a key is per-PROVIDER (one key per supplier), independent of
 * which model a turn uses. Model selection happens at conversation time (the composer's
 * picker), NOT here.
 *
 * Layout: a single "add a key" row (provider dropdown + password field + validate/save),
 * then a list of already-configured keys below, each with its save time and a delete
 * button. On save we PROBE the key with a minimal request (validateKey) and only persist
 * it if it actually works — so a bad paste is caught here, not on the first real turn.
 */
import { common, Want } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { BUILTIN_PROVIDERS, ProviderRecord } from '../model/Providers';
import { SecureKeyStore } from '../core/SecureKeyStore';
import { ConfigStore } from '../core/ConfigStore';
import { validateKey, ValidationResult } from '../core/Transport';

interface ConfiguredKey {
  providerId: string;
  label: string;
  registerUrl: string;
  savedAt: number;
}

@CustomDialog
export struct ProviderManagerDialog {
  controller: CustomDialogController;
  onDone: () => void = () => {};
  // Called after a key is validated & saved, so the host can hand off to the model-config
  // dialog for the just-saved provider.
  onConfigureModels: (providerId: string) => void = () => {};

  @State private selectedIndex: number = 0;
  @State private draft: string = '';
  @State private busy: boolean = false;
  @State private result: string = '';
  @State private configured: ConfiguredKey[] = [];

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  async aboutToAppear(): Promise<void> {
    await this.refreshList();
  }

  private selectedProvider(): ProviderRecord {
    return BUILTIN_PROVIDERS[this.selectedIndex];
  }

  private options(): SelectOption[] {
    const opts: SelectOption[] = [];
    for (const p of BUILTIN_PROVIDERS) {
      opts.push({ value: p.label });
    }
    return opts;
  }

  private async refreshList(): Promise<void> {
    const list: ConfiguredKey[] = [];
    for (const p of BUILTIN_PROVIDERS) {
      if (await SecureKeyStore.hasKey(p.id)) {
        const ts = await ConfigStore.getKeyTime(this.ctx(), p.id);
        list.push({ providerId: p.id, label: p.label, registerUrl: p.registerUrl, savedAt: ts });
      }
    }
    this.configured = list;
  }

  private async onSaveKey(): Promise<void> {
    const p = this.selectedProvider();
    const k = this.draft.trim();
    if (k.length === 0) {
      promptAction.showToast({ message: this.rs($r('app.string.provider_paste_key_first')), duration: 2000 });
      return;
    }
    this.busy = true;
    this.result = this.rs($r('app.string.provider_validating'));

    let res: ValidationResult;
    try {
      res = await validateKey(p, k);
    } catch (err) {
      res = { ok: false, code: 0, message: this.rs($r('app.string.provider_network_error')) };
    }

    if (res.ok) {
      try {
        await SecureKeyStore.saveKey(p.id, k);
        await ConfigStore.setKeyTime(this.ctx(), p.id, Date.now());
        this.draft = '';
        await this.refreshList();
        this.busy = false;
        // Validated & saved — close and hand off to model configuration for this service.
        this.controller.close();
        this.onConfigureModels(p.id);
        return;
      } catch (err) {
        this.result = this.rs($r('app.string.provider_save_failed'));
      }
    } else if (res.code === 401 || res.code === 403) {
      this.result = this.rs($r('app.string.provider_key_invalid'));
    } else {
      // Unmapped: show the provider's own error detail (already language-neutral-ish).
      this.result = '✗ ' + res.message;
    }
    this.busy = false;
  }

  private async onDelete(providerId: string): Promise<void> {
    try {
      await SecureKeyStore.deleteKey(providerId);
      await ConfigStore.clearKeyTime(this.ctx(), providerId);
      // Deleting the service invalidates every model added under it — drop them too, so the
      // composer picker can't offer a model whose key is gone.
      await ConfigStore.removeAddedModelsForProvider(this.ctx(), providerId);
      await this.refreshList();
    } catch (err) {
      promptAction.showToast({ message: this.rs($r('app.string.provider_delete_failed')), duration: 2000 });
    }
  }

  private openRegister(url: string): void {
    const want: Want = { action: 'ohos.want.action.viewData', uri: url };
    this.ctx().startAbility(want).catch(() => {
      promptAction.showToast({ message: this.rs($r('app.string.provider_open_browser_failed')), duration: 2000 });
    });
  }

  private pad2(n: number): string {
    return n < 10 ? '0' + n : '' + n;
  }

  private formatTime(ts: number): string {
    if (ts <= 0) {
      return this.rs($r('app.string.provider_time_unknown'));
    }
    const d = new Date(ts);
    return d.getFullYear() + '-' + this.pad2(d.getMonth() + 1) + '-' + this.pad2(d.getDate()) +
      ' ' + this.pad2(d.getHours()) + ':' + this.pad2(d.getMinutes());
  }

  @Builder
  private configuredRow(c: ConfiguredKey) {
    Row() {
      Column({ space: 2 }) {
        Text(c.label)
          .fontSize(15)
          .fontWeight(FontWeight.Medium)
        Text(this.rs($r('app.string.provider_configured_at'), this.formatTime(c.savedAt)))
          .fontSize(12)
          .fontColor('#9A9A9A')
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Button({ type: ButtonType.Circle }) {
        Text('－').fontSize(18).fontColor(Color.White).fontWeight(FontWeight.Bold)
      }
      .width(28)
      .height(28)
      .backgroundColor('#D24C57')
      .onClick(() => {
        this.onDelete(c.providerId);
      })
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
    .padding({ top: 13, bottom: 13 })
    .border({ width: { bottom: 0.5 }, color: '#EEEEEE' })
  }

  build() {
    Column() {
      Row() {
        Text($r('app.string.provider_dialog_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        Text('✕')
          .fontSize(20)
          .fontColor('#8A8A8A')
          .padding({ left: 12, top: 2, bottom: 2 })
          .onClick(() => {
            this.controller.close();
            this.onDone();
          })
      }
      .width('100%')
      .margin({ bottom: 6 })
      .alignItems(VerticalAlign.Center)
      Text($r('app.string.provider_intro'))
        .fontSize(12)
        .fontColor('#8A8A8A')
        .width('100%')
        .margin({ bottom: 12 })

      // ── Add a key ──────────────────────────────────────────────
      Text($r('app.string.provider_add_key'))
        .fontSize(13)
        .fontColor('#8A8A8A')
        .width('100%')
        .margin({ bottom: 6 })

      Select(this.options())
        .selected(this.selectedIndex)
        .value(this.selectedProvider().label)
        .width('100%')
        .onSelect((index: number) => {
          this.selectedIndex = index;
          this.result = '';
        })

      TextInput({ text: this.draft, placeholder: this.rs($r('app.string.provider_paste_hint'), this.selectedProvider().label) })
        .type(InputType.Password)
        .width('100%')
        .margin({ top: 8 })
        .enabled(!this.busy)
        .onChange((v: string) => {
          this.draft = v;
        })

      Row({ space: 10 }) {
        Button(this.busy ? $r('app.string.provider_validating_btn') : $r('app.string.provider_validate_save'))
          .layoutWeight(1)
          .enabled(!this.busy)
          .onClick(() => {
            this.onSaveKey();
          })
        Text($r('app.string.provider_register'))
          .fontSize(13)
          .fontColor('#3F7FC1')
          .onClick(() => {
            this.openRegister(this.selectedProvider().registerUrl);
          })
        if (this.busy) {
          LoadingProgress().width(22).height(22)
        }
      }
      .width('100%')
      .margin({ top: 8 })
      .alignItems(VerticalAlign.Center)

      if (this.result.length > 0) {
        Text(this.result)
          .fontSize(12)
          .fontColor(this.result.startsWith('✗') ? '#D24C57' : '#46A86A')
          .width('100%')
          .margin({ top: 6 })
      }

      // ── Configured keys ────────────────────────────────────────
      Text(this.rs($r('app.string.provider_configured_count'), this.configured.length))
        .fontSize(13)
        .fontColor('#8A8A8A')
        .width('100%')
        .margin({ top: 16, bottom: 4 })

      if (this.configured.length === 0) {
        Text($r('app.string.provider_no_keys'))
          .fontSize(13)
          .fontColor('#B0B0B0')
          .width('100%')
          .padding({ top: 13, bottom: 13 })
      } else {
        Scroll() {
          Column() {
            ForEach(this.configured, (c: ConfiguredKey) => {
              this.configuredRow(c)
            }, (c: ConfiguredKey) => c.providerId)
          }
          .width('100%')
        }
        .constraintSize({ maxHeight: 240 })
        .width('100%')
      }

    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

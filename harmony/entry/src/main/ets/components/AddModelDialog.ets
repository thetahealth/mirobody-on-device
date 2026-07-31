/**
 * Interface 2 — Add models to the composer dropdown.
 *
 * Pick a provider that has a key, see its LIVE model list (authoritative /v1/models joined
 * with the website catalog for 🆓 free flag / call volume / deprecation date), search, and
 * add the ones you want. The composer dropdown then shows only the models added here.
 *
 * Both data sources are fetched live (see Transport.discoverModels) — nothing is baked in.
 */
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import { BUILTIN_PROVIDERS, ProviderRecord } from '../model/Providers';
import { SecureKeyStore } from '../core/SecureKeyStore';
import { ConfigStore, AddedModel } from '../core/ConfigStore';
import { discoverModels, DiscoveredModel, DiscoveryResult } from '../core/Transport';

@CustomDialog
export struct AddModelDialog {
  controller: CustomDialogController;
  onDone: () => void = () => {};

  @State private configured: ProviderRecord[] = [];
  @State private selectedIdx: number = 0;
  @State private loading: boolean = false;
  @State private models: DiscoveredModel[] = [];
  @State private search: string = '';
  @State private added: AddedModel[] = [];
  @State private onlyFree: boolean = true;
  // Why the live listing failed, if it did (0 = fine). Kept separate from `models` because a
  // failure can still leave the provider's curated ids to show — that list is real but partial.
  @State private errCode: number = 0;
  @State private errMessage: string = '';

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  async aboutToAppear(): Promise<void> {
    const list: ProviderRecord[] = [];
    for (const p of BUILTIN_PROVIDERS) {
      if (await SecureKeyStore.hasKey(p.id)) {
        list.push(p);
      }
    }
    this.configured = list;
    // Default to the most-recently-saved provider so that saving a key and landing here opens
    // straight on that service (also a sensible default when reopened manually).
    this.selectedIdx = await this.latestConfiguredIdx(list);
    this.added = await ConfigStore.getAddedModels(this.ctx());
    if (list.length > 0) {
      await this.loadModels();
    }
  }

  // Index (into `configured`) of the provider whose key was saved most recently; 0 if none.
  private async latestConfiguredIdx(list: ProviderRecord[]): Promise<number> {
    let bestIdx = 0;
    let bestTs = -1;
    for (let i = 0; i < list.length; i++) {
      const ts = await ConfigStore.getKeyTime(this.ctx(), list[i].id);
      if (ts > bestTs) {
        bestTs = ts;
        bestIdx = i;
      }
    }
    return bestIdx;
  }

  private selectedProvider(): ProviderRecord | undefined {
    return this.selectedIdx < this.configured.length ? this.configured[this.selectedIdx] : undefined;
  }

  private options(): SelectOption[] {
    const opts: SelectOption[] = [];
    for (const p of this.configured) {
      opts.push({ value: p.label });
    }
    return opts;
  }

  private async loadModels(): Promise<void> {
    const p = this.selectedProvider();
    if (p === undefined) {
      return;
    }
    this.loading = true;
    this.models = [];
    this.errCode = 0;
    this.errMessage = '';
    const key = await SecureKeyStore.getKey(p.id);
    const res: DiscoveryResult = await discoverModels(p, key);
    const found = res.models;
    found.sort((a: DiscoveredModel, b: DiscoveredModel) => {
      if (a.free !== b.free) {
        return a.free ? -1 : 1; // free first
      }
      if (b.popularity !== a.popularity) {
        return b.popularity - a.popularity;
      }
      return a.displayName.localeCompare(b.displayName);
    });
    this.models = found;
    this.errCode = res.errorCode;
    this.errMessage = res.errorMessage;
    this.loading = false;
  }

  // One line explaining errCode. 401/403 reuses the same wording the chat surface uses, so a
  // dead key reads identically wherever the user hits it.
  private errText(): string {
    if (this.errCode === 401 || this.errCode === 403) {
      return this.rs($r('app.string.chat_err_key'));
    }
    if (this.errCode < 0) {
      return this.rs($r('app.string.model_fetch_offline'));
    }
    if (this.errMessage.length > 0) {
      return this.errMessage;
    }
    return this.rs($r('app.string.chat_err_request'), this.errCode);
  }

  private isAdded(providerId: string, id: string): boolean {
    for (const a of this.added) {
      if (a.providerId === providerId && a.id === id) {
        return true;
      }
    }
    return false;
  }

  // Whether model `id` is added under the currently selected provider.
  private addedForCurrent(id: string): boolean {
    const p = this.selectedProvider();
    return p !== undefined && this.isAdded(p.id, id);
  }

  private async toggle(m: DiscoveredModel): Promise<void> {
    const p = this.selectedProvider();
    if (p === undefined) {
      return;
    }
    const next: AddedModel[] = [];
    let removed = false;
    for (const a of this.added) {
      if (a.providerId === p.id && a.id === m.id) {
        removed = true;
      } else {
        next.push(a);
      }
    }
    if (!removed) {
      next.push({ providerId: p.id, id: m.id, displayName: m.displayName });
    }
    this.added = next;
    await ConfigStore.setAddedModels(this.ctx(), next);
    promptAction.showToast({
      message: this.rs(removed ? $r('app.string.toast_removed') : $r('app.string.toast_added'), m.displayName),
      duration: 1800
    });
  }

  private fmtPop(n: number): string {
    if (n >= 1000000) {
      return (n / 1000000).toFixed(1) + 'M';
    }
    if (n >= 1000) {
      return (n / 1000).toFixed(0) + 'K';
    }
    return n.toString();
  }

  private hasAnyFree(): boolean {
    for (const m of this.models) {
      if (m.free) {
        return true;
      }
    }
    return false;
  }

  // True when the list is currently filtered to free-only (so per-row free marks are moot).
  private freeOnlyActive(): boolean {
    return this.onlyFree && this.hasAnyFree();
  }

  private filtered(): DiscoveredModel[] {
    const q = this.search.trim().toLowerCase();
    // "only free" applies only when we actually have free-flag data for this provider;
    // otherwise (no catalog) it would hide everything, so we show all.
    const freeOnly = this.onlyFree && this.hasAnyFree();
    const out: DiscoveredModel[] = [];
    for (const m of this.models) {
      if (freeOnly && !m.free) {
        continue;
      }
      if (q.length > 0 && !(m.id.toLowerCase().includes(q) || m.displayName.toLowerCase().includes(q))) {
        continue;
      }
      out.push(m);
    }
    return out;
  }

  @Builder
  private modelRow(m: DiscoveredModel) {
    Row() {
      Checkbox()
        .select(this.addedForCurrent(m.id))
        .selectedColor($r('app.color.brand'))
        .shape(CheckBoxShape.ROUNDED_SQUARE)
        .width(22)
        .height(22)
        .margin({ right: 10 })
        .onChange((checked: boolean) => {
          if (checked !== this.addedForCurrent(m.id)) {
            this.toggle(m);
          }
        })
      Column({ space: 2 }) {
        Row({ space: 6 }) {
          // Flag non-free models with a red "!" — only when this provider actually
          // distinguishes free vs paid (else, e.g. Gemini, everything would get flagged).
          if (this.hasAnyFree() && !this.onlyFree && !m.free) {
            Text('!')
              .fontSize(11)
              .fontColor($r('app.color.on_brand'))
              .fontWeight(FontWeight.Bold)
              .backgroundColor($r('app.color.danger'))
              .borderRadius(9)
              .padding({ left: 6, right: 6, top: 1, bottom: 1 })
          }
          Text(m.displayName)
            .fontSize(15)
            .fontWeight(FontWeight.Medium)
            .layoutWeight(1)
        }

        Row({ space: 8 }) {
          if (m.popularity > 0) {
            Text(this.rs($r('app.string.model_calls'), this.fmtPop(m.popularity)))
              .fontSize(11)
              .fontColor($r('app.color.text_tertiary'))
          }
          if (m.deprecation.length > 0) {
            Text(this.rs($r('app.string.model_retires'), m.deprecation))
              .fontSize(11)
              .fontColor($r('app.color.warn'))
          }
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
    .padding({ top: 13, bottom: 13, left: 4, right: 24 })
    .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
  }

  build() {
    Column() {
      Row() {
        Text($r('app.string.model_dialog_title'))
          .fontSize(18)
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        Text('✕')
          .fontSize(20)
          .fontColor($r('app.color.text_tertiary'))
          .padding({ left: 12, top: 2, bottom: 2 })
          .onClick(() => {
            this.controller.close();
            this.onDone();
          })
      }
      .width('100%')
      .margin({ bottom: 6 })
      .alignItems(VerticalAlign.Center)

      if (this.configured.length === 0) {
        Text($r('app.string.model_no_keys'))
          .fontSize(13)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%')
          .padding({ top: 12, bottom: 12 })
      } else {
        Select(this.options())
          .selected(this.selectedIdx)
          .value(this.selectedProvider()?.label ?? '')
          .width('100%')
          .onSelect((index: number) => {
            this.selectedIdx = index;
            this.search = '';
            this.loadModels();
          })

        // Search + "only free" on one row. The toggle is hidden when this provider exposes
        // no free-flag data (would otherwise hide everything).
        Row({ space: 10 }) {
          TextInput({ text: this.search, placeholder: $r('app.string.model_search_hint') })
            .layoutWeight(1)
            .height(40)
            .onChange((v: string) => {
              this.search = v;
            })
          if (this.hasAnyFree()) {
            Toggle({ type: ToggleType.Button, isOn: this.onlyFree }) {
              Text($r('app.string.model_free'))
                .fontSize(16)
                .fontColor(this.onlyFree ? $r('app.color.on_brand') : $r('app.color.text_tertiary'))
                .padding({ left: 16, right: 16 })
            }
            .height(40)
            .selectedColor($r('app.color.success'))
            .onChange((on: boolean) => {
              this.onlyFree = on;
            })
          }
        }
        .width('100%')
        .margin({ top: 8 })
        .alignItems(VerticalAlign.Center)

        // Fixed-height results area so the dialog doesn't jump when the async list arrives.
        if (this.loading) {
          Row({ space: 8 }) {
            LoadingProgress().width(22).height(22)
            Text($r('app.string.model_loading')).fontSize(13).fontColor($r('app.color.text_tertiary'))
          }
          .width('100%')
          .height(380)
          .margin({ top: 8 })
          .justifyContent(FlexAlign.Center)
        } else if (this.errCode !== 0 && this.models.length === 0) {
          // Total failure — nothing to fall back on. Say why and offer a retry, rather than
          // rendering an empty box the user can't distinguish from "no models here".
          Column({ space: 10 }) {
            Text($r('app.string.model_fetch_failed'))
              .fontSize(15)
              .fontWeight(FontWeight.Medium)
              .fontColor($r('app.color.text_primary'))
            Text(this.errText())
              .fontSize(13)
              .fontColor($r('app.color.text_tertiary'))
              .textAlign(TextAlign.Center)
            Button($r('app.string.model_retry'))
              .height(36)
              .backgroundColor($r('app.color.brand'))
              .onClick(() => {
                this.loadModels();
              })
          }
          .width('100%')
          .height(380)
          .margin({ top: 8 })
          .padding({ left: 16, right: 16 })
          .justifyContent(FlexAlign.Center)
        } else {
          // Partial failure — the curated fallback is showing, so the list is real but
          // incomplete. Flag that instead of passing it off as the full catalog.
          if (this.errCode !== 0) {
            Row({ space: 8 }) {
              Text($r('app.string.model_fetch_partial'))
                .fontSize(12)
                .fontColor($r('app.color.notice_fg'))
                .layoutWeight(1)
              Text($r('app.string.model_retry'))
                .fontSize(12)
                .fontColor($r('app.color.brand_text'))
                .onClick(() => {
                  this.loadModels();
                })
            }
            .width('100%')
            .margin({ top: 8 })
            .padding({ left: 12, right: 12, top: 8, bottom: 8 })
            .borderRadius(8)
            .backgroundColor($r('app.color.notice_bg'))
            .alignItems(VerticalAlign.Center)
          }

          Scroll() {
            Column() {
              if (this.filtered().length === 0) {
                Text(this.models.length === 0
                  ? $r('app.string.model_empty')
                  : $r('app.string.model_no_match'))
                  .fontSize(13)
                  .fontColor($r('app.color.text_tertiary'))
                  .width('100%')
                  .textAlign(TextAlign.Center)
                  .padding({ top: 40, bottom: 40 })
              }
              ForEach(this.filtered(), (m: DiscoveredModel) => {
                this.modelRow(m)
              }, (m: DiscoveredModel) => m.id)
            }
            .width('100%')
          }
          .height(380)
          .align(Alignment.Top)
          .width('100%')
          .margin({ top: 8 })
        }
      }

    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

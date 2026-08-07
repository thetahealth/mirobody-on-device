/**
 * 华为运动健康 (Huawei Health) data source — the one health screen this client has.
 *
 * Four bands, in the order the questions arise: what this does and where the data
 * goes; whether we are authorized (and the button that asks); which metrics and
 * how far back; then what is actually stored, read back from the device so it
 * survives a relaunch instead of only showing what this session just wrote.
 *
 * The authorization state is NOT hidden behind the sync button. Health Service
 * Kit needs an approved AppGallery Connect entitlement before it will hand over
 * anything (see core/HealthSource.ets), so "unavailable" and "not authorized yet"
 * are states the user can genuinely be in — and the Kit's own error text is shown
 * verbatim, because on this path it is the only thing that says which of the two
 * it is.
 */
import { common } from '@kit.AbilityKit';
import { HEALTH_METRICS, HealthMetricRecord } from '../model/HealthMetrics';
import { HealthRepository, SyncOutcome, HealthEntry, HealthRecent } from '../core/HealthRepository';
import { HealthStatus } from '../core/HealthSource';
import { FONT, ICON } from '../model/Typography';

interface WindowOpt {
  days: number;
  labelRes: Resource;
}

@CustomDialog
export struct HealthDataDialog {
  controller: CustomDialogController;
  onDone: () => void = () => {};

  @State private available: boolean = true;
  @State private granted: string[] = [];
  @State private kitError: string = '';
  @State private enabledSlugs: string[] = [];
  @State private days: number = 7;
  @State private lastSync: number = 0;
  @State private lastStored: number = 0;
  @State private busy: boolean = false;
  @State private result: string = '';
  @State private recent: HealthEntry[] = [];
  @State private total: number = 0;

  private windows: WindowOpt[] = [
    { days: 7, labelRes: $r('app.string.health_window_7') },
    { days: 30, labelRes: $r('app.string.health_window_30') },
    { days: 90, labelRes: $r('app.string.health_window_90') }
  ];

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  async aboutToAppear(): Promise<void> {
    this.enabledSlugs = await HealthRepository.enabledSlugs(this.ctx());
    this.days = await HealthRepository.windowDays(this.ctx());
    this.lastSync = await HealthRepository.lastSync(this.ctx());
    this.lastStored = await HealthRepository.lastStored(this.ctx());
    await this.refreshStatus();
    await this.refreshStored();
  }

  private async refreshStatus(): Promise<void> {
    const st: HealthStatus = await HealthRepository.status(this.ctx());
    this.available = st.available;
    this.granted = st.granted;
    this.kitError = st.error;
  }

  /** What the on-device store already holds — the proof a sync landed. */
  private async refreshStored(): Promise<void> {
    const r: HealthRecent = await HealthRepository.recent(this.ctx(), 20);
    this.total = r.total;
    this.recent = r.items;
  }

  private isEnabled(slug: string): boolean {
    return this.enabledSlugs.includes(slug);
  }

  private async toggle(slug: string): Promise<void> {
    const next: string[] = [];
    for (const s of this.enabledSlugs) {
      if (s !== slug) {
        next.push(s);
      }
    }
    if (next.length === this.enabledSlugs.length) {
      next.push(slug);
    }
    this.enabledSlugs = next;
    await HealthRepository.setEnabledSlugs(this.ctx(), next);
    // Authorization is per data type, so changing the selection changes what is
    // granted — re-read it rather than leave a stale "authorized" badge behind.
    await this.refreshStatus();
  }

  private async pickWindow(days: number): Promise<void> {
    this.days = days;
    await HealthRepository.setWindowDays(this.ctx(), days);
  }

  private async onAuthorize(): Promise<void> {
    this.busy = true;
    this.result = '';
    const st: HealthStatus = await HealthRepository.authorize(this.ctx());
    this.available = st.available;
    this.granted = st.granted;
    this.kitError = st.error;
    this.busy = false;
    if (st.granted.length === 0 && st.error.length > 0) {
      this.result = '✗ ' + st.error;
    }
  }

  private async onSync(): Promise<void> {
    this.busy = true;
    this.result = this.rs($r('app.string.health_syncing'));
    const out: SyncOutcome = await HealthRepository.sync(this.ctx());
    this.busy = false;

    if (out.stored > 0) {
      this.lastSync = await HealthRepository.lastSync(this.ctx());
      this.lastStored = out.stored;
      // A partial success is still a success worth naming, with the failure beside
      // it: "12 stored" alone would hide a scope that silently returned nothing.
      this.result = this.rs($r('app.string.health_sync_ok'), out.stored) +
        (out.error.length > 0 ? ' · ' + out.error : '');
      await this.refreshStored();
    } else if (out.error.length > 0) {
      this.result = '✗ ' + out.error;
    } else {
      this.result = this.rs($r('app.string.health_sync_empty'));
    }
  }

  private pad2(n: number): string {
    return n < 10 ? '0' + n : '' + n;
  }

  private formatTime(ts: number): string {
    if (ts <= 0) {
      return this.rs($r('app.string.health_never_synced'));
    }
    const d = new Date(ts);
    return d.getFullYear() + '-' + this.pad2(d.getMonth() + 1) + '-' + this.pad2(d.getDate()) +
      ' ' + this.pad2(d.getHours()) + ':' + this.pad2(d.getMinutes());
  }

  /** "2026-08-05 07:30" out of the Observation's ISO instant, without a parser. */
  private shortInstant(iso: string): string {
    if (iso.length < 16) {
      return iso;
    }
    return iso.substring(0, 10) + ' ' + iso.substring(11, 16);
  }

  private valueText(e: HealthEntry): string {
    const rounded = Math.round(e.value * 10) / 10;
    return rounded.toString() + (e.unit.length > 0 ? ' ' + e.unit : '');
  }

  @Builder
  private metricRow(m: HealthMetricRecord) {
    Row() {
      Checkbox()
        .select(this.isEnabled(m.slug))
        .selectedColor($r('app.color.brand'))
        .shape(CheckBoxShape.ROUNDED_SQUARE)
        .width(22)
        .height(22)
        .margin({ right: 10 })
        .onChange((checked: boolean) => {
          if (checked !== this.isEnabled(m.slug)) {
            this.toggle(m.slug);
          }
        })
      Text(m.labelRes)
        .fontSize(FONT.row)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
      // Per-metric grant state: authorization is granted per data type, so one
      // metric can be readable while its neighbour is not.
      if (this.granted.includes(m.slug)) {
        Text('✓')
          .fontSize(FONT.body)
          .fontColor($r('app.color.success'))
      }
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
    .padding({ top: 11, bottom: 11, left: 4 })
    .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
  }

  @Builder
  private storedRow(e: HealthEntry) {
    Row() {
      Column({ space: 2 }) {
        Text(e.display.length > 0 ? e.display : e.code)
          .fontSize(FONT.body)
          .fontColor($r('app.color.text_primary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(this.shortInstant(e.time))
          .fontSize(FONT.footnote)
          .fontColor($r('app.color.text_tertiary'))
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text(this.valueText(e))
        .fontSize(FONT.body)
        .fontWeight(FontWeight.Medium)
        .fontColor($r('app.color.brand_text'))
    }
    .width('100%')
    .alignItems(VerticalAlign.Center)
    .padding({ top: 10, bottom: 10 })
    .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
  }

  build() {
    Column() {
      Row() {
        Text($r('app.string.health_dialog_title'))
          .fontSize(FONT.title)
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        Text('✕')
          .fontSize(ICON.lg)
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

      Text($r('app.string.health_intro'))
        .fontSize(FONT.caption)
        .fontColor($r('app.color.text_tertiary'))
        .width('100%')
        .margin({ bottom: 12 })

      // ── Authorization ──────────────────────────────────────────
      if (!this.available) {
        // No Health Service Kit on this device / build: say so plainly and stop.
        // Nothing below would do anything.
        Column({ space: 6 }) {
          Text($r('app.string.health_unavailable'))
            .fontSize(FONT.label)
            .fontColor($r('app.color.warn_text'))
            .width('100%')
          if (this.kitError.length > 0) {
            Text(this.kitError)
              .fontSize(FONT.footnote)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
          }
        }
        .width('100%')
        .padding({ top: 6, bottom: 6 })
      } else {
        Row({ space: 10 }) {
          Text(this.granted.length > 0
            ? this.rs($r('app.string.health_authorized'), this.granted.length)
            : this.rs($r('app.string.health_not_authorized')))
            .fontSize(FONT.label)
            .fontColor(this.granted.length > 0 ? $r('app.color.success') : $r('app.color.text_tertiary'))
            .layoutWeight(1)
          Button($r('app.string.health_authorize'))
            .height(34)
            .fontSize(FONT.body)
            .enabled(!this.busy)
            .onClick(() => {
              this.onAuthorize();
            })
        }
        .width('100%')
        .alignItems(VerticalAlign.Center)

        // ── Metrics ──────────────────────────────────────────────
        Text($r('app.string.health_metrics'))
          .fontSize(FONT.label)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%')
          .margin({ top: 14, bottom: 2 })

        Scroll() {
          Column() {
            ForEach(HEALTH_METRICS, (m: HealthMetricRecord) => {
              this.metricRow(m)
            }, (m: HealthMetricRecord) => m.slug +
                 (this.isEnabled(m.slug) ? '#e' : '') + (this.granted.includes(m.slug) ? '#g' : ''))
          }
          .width('100%')
        }
        .constraintSize({ maxHeight: 220 })
        .width('100%')

        // ── Window ───────────────────────────────────────────────
        Row({ space: 8 }) {
          Text($r('app.string.health_window'))
            .fontSize(FONT.label)
            .fontColor($r('app.color.text_tertiary'))
          ForEach(this.windows, (w: WindowOpt) => {
            Text(w.labelRes)
              .fontSize(FONT.label)
              .fontColor(this.days === w.days ? $r('app.color.on_brand') : $r('app.color.text_secondary'))
              .backgroundColor(this.days === w.days ? $r('app.color.brand') : Color.Transparent)
              .borderRadius(14)
              .padding({ left: 12, right: 12, top: 5, bottom: 5 })
              .onClick(() => {
                this.pickWindow(w.days);
              })
          }, (w: WindowOpt) => w.days.toString() + (this.days === w.days ? '#a' : ''))
        }
        .width('100%')
        .margin({ top: 12 })
        .alignItems(VerticalAlign.Center)

        // ── Sync ─────────────────────────────────────────────────
        Row({ space: 10 }) {
          Button(this.busy ? $r('app.string.health_sync_btn_busy') : $r('app.string.health_sync_btn'))
            .layoutWeight(1)
            .enabled(!this.busy && this.granted.length > 0)
            .onClick(() => {
              this.onSync();
            })
          if (this.busy) {
            LoadingProgress().width(22).height(22)
          }
        }
        .width('100%')
        .margin({ top: 12 })
        .alignItems(VerticalAlign.Center)

        Text(this.lastStored > 0
          ? this.rs($r('app.string.health_last_sync_n'), this.formatTime(this.lastSync), this.lastStored)
          : this.formatTime(this.lastSync))
          .fontSize(FONT.footnote)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%')
          .margin({ top: 6 })

        if (this.result.length > 0) {
          Text(this.result)
            .fontSize(FONT.caption)
            .fontColor(this.result.startsWith('✗') ? $r('app.color.danger') : $r('app.color.success'))
            .width('100%')
            .margin({ top: 4 })
        }

        // ── What is stored ───────────────────────────────────────
        Text(this.rs($r('app.string.health_stored_count'), this.total))
          .fontSize(FONT.label)
          .fontColor($r('app.color.text_tertiary'))
          .width('100%')
          .margin({ top: 14, bottom: 2 })

        if (this.recent.length === 0) {
          Text($r('app.string.health_stored_empty'))
            .fontSize(FONT.label)
            .fontColor($r('app.color.text_disabled'))
            .width('100%')
            .padding({ top: 10, bottom: 10 })
        } else {
          Scroll() {
            Column() {
              ForEach(this.recent, (e: HealthEntry) => {
                this.storedRow(e)
              }, (e: HealthEntry) => e.code + '#' + e.time)
            }
            .width('100%')
          }
          .constraintSize({ maxHeight: 200 })
          .width('100%')
        }
      }
    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

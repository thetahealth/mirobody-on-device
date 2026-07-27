/**
 * App settings dialogs — the top-right gear menu (mirrors the Android AppSettingsMenu:
 * Language / Font size / About). Backend is omitted (BYOK has no single backend server;
 * per-provider base_url lives in 配置云服务).
 */
import { i18n } from '@kit.LocalizationKit';
import { bundleManager } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';

interface LangOpt {
  label: string;
  tag: string;
}

@Builder
function dialogHeader(title: ResourceStr, close: () => void) {
  Row() {
    Text(title)
      .fontSize(18)
      .fontWeight(FontWeight.Bold)
      .layoutWeight(1)
    Text('✕')
      .fontSize(20)
      .fontColor('#8A8A8A')
      .padding({ left: 12, top: 2, bottom: 2 })
      .onClick(close)
  }
  .width('100%')
  .margin({ bottom: 8 })
  .alignItems(VerticalAlign.Center)
}

@CustomDialog
export struct LanguageDialog {
  controller: CustomDialogController;
  private opts: LangOpt[] = [
    { label: '简体中文', tag: 'zh-Hans' },
    { label: 'English', tag: 'en' }
  ];

  private rs(res: Resource): string {
    return getContext(this).resourceManager.getStringSync(res.id);
  }

  private apply(tag: string): void {
    try {
      i18n.System.setAppPreferredLanguage(tag);
      promptAction.showToast({ message: this.rs($r('app.string.settings_lang_switched')), duration: 2600 });
    } catch (err) {
      promptAction.showToast({ message: this.rs($r('app.string.settings_lang_switch_failed')), duration: 2000 });
    }
    this.controller.close();
  }

  build() {
    Column() {
      dialogHeader($r('app.string.settings_language'), () => {
        this.controller.close();
      })
      ForEach(this.opts, (o: LangOpt) => {
        Row() {
          Text(o.label).fontSize(16).layoutWeight(1)
        }
        .width('100%')
        .padding({ top: 13, bottom: 13 })
        .border({ width: { bottom: 0.5 }, color: '#EEEEEE' })
        .onClick(() => {
          this.apply(o.tag);
        })
      }, (o: LangOpt) => o.tag)
    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

@CustomDialog
export struct FontSizeDialog {
  controller: CustomDialogController;
  @StorageLink('fontScale') fontScale: number = 1;
  @State private idx: number = 2;
  // 5 tiers, mirrors Android's slider (更小 / 小 / 标准 / 大 / 更大).
  private scales: number[] = [0.8, 0.9, 1.0, 1.15, 1.3];
  private labels: ResourceStr[] = [
    $r('app.string.font_smaller'),
    $r('app.string.font_small'),
    $r('app.string.font_standard'),
    $r('app.string.font_large'),
    $r('app.string.font_larger')
  ];

  aboutToAppear(): void {
    let best = 2;
    let bestD = 999;
    for (let i = 0; i < this.scales.length; i++) {
      const d = Math.abs(this.scales[i] - this.fontScale);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    this.idx = best;
  }

  build() {
    Column({ space: 12 }) {
      dialogHeader($r('app.string.settings_font_size'), () => {
        this.controller.close();
      })

      // Live preview — reflects the current scale as you drag.
      Text($r('app.string.font_preview'))
        .fontSize(16 * this.fontScale)
        .fontColor('#1A1A1A')
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 4, bottom: 4 })

      Slider({ value: this.idx, min: 0, max: 4, step: 1, style: SliderStyle.OutSet })
        .width('100%')
        .blockColor('#FFFFFF')
        .selectedColor('#3F7FC1')
        .trackThickness(4)
        .showSteps(true)
        .onChange((value: number) => {
          const i = Math.round(value);
          if (i !== this.idx) {
            this.idx = i;
            this.fontScale = this.scales[i];
          }
        })

      Row() {
        ForEach(this.labels, (lab: ResourceStr, i: number) => {
          Text(lab)
            .fontSize(12)
            .fontColor(i === this.idx ? '#3F7FC1' : '#9A9A9A')
        }, (lab: ResourceStr, i: number) => i.toString())
      }
      .width('100%')
      .justifyContent(FlexAlign.SpaceBetween)
    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

@CustomDialog
export struct AboutDialog {
  controller: CustomDialogController;
  @State private version: string = '';

  aboutToAppear(): void {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(
        bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      this.version = info.versionName;
    } catch (err) {
      this.version = '';
    }
  }

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  build() {
    Column({ space: 6 }) {
      dialogHeader($r('app.string.settings_about'), () => {
        this.controller.close();
      })
      Text($r('app.string.app_name'))
        .fontSize(17)
        .fontWeight(FontWeight.Medium)
        .width('100%')
      Text(this.rs($r('app.string.about_version'), this.version.length > 0 ? this.version : '—'))
        .fontSize(14)
        .fontColor('#8A8A8A')
        .width('100%')
      Text($r('app.string.about_desc'))
        .fontSize(13)
        .fontColor('#9A9A9A')
        .width('100%')
        .margin({ top: 4 })
    }
    .width('100%')
    .padding(24)
    .backgroundColor(Color.Transparent)
    .borderRadius(16)
  }
}

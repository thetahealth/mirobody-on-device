/**
 * App settings dialogs, opened from the nav drawer's settings group (mirrors Android's
 * AppSettingsSection: Language / Font size / Appearance). Backend is omitted —
 * BYOK has no single backend server; per-provider base_url lives in 配置云服务.
 */
import { i18n } from '@kit.LocalizationKit';
import { common } from '@kit.AbilityKit';
import { promptAction } from '@kit.ArkUI';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';

interface LangOpt {
  label: string;
  tag: string;
}

interface ThemeOpt {
  label: ResourceStr;
  mode: number; // ConfigurationConstant.ColorMode
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
      .fontColor($r('app.color.text_tertiary'))
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
        .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
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

/**
 * Theme override: follow the system, or pin light/dark. The stored value IS the ColorMode
 * enum value, so there is no mapping table. Colors swap through resources/dark automatically;
 * the offscreen render host follows via the 'darkMode' flag EntryAbility publishes when the
 * configuration update lands.
 */
@CustomDialog
export struct ThemeDialog {
  controller: CustomDialogController;
  // Persisted in EntryAbility; writing through @StorageLink survives the next launch.
  @StorageLink('colorModePref') pref: number = ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;

  private opts: ThemeOpt[] = [
    { label: $r('app.string.theme_system'), mode: ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET },
    { label: $r('app.string.theme_light'), mode: ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT },
    { label: $r('app.string.theme_dark'), mode: ConfigurationConstant.ColorMode.COLOR_MODE_DARK }
  ];

  private apply(mode: number): void {
    const ctx = getContext(this) as common.UIAbilityContext;
    ctx.getApplicationContext().setColorMode(mode as ConfigurationConstant.ColorMode);
    this.pref = mode;
    this.controller.close();
  }

  build() {
    Column() {
      dialogHeader($r('app.string.settings_theme'), () => {
        this.controller.close();
      })
      ForEach(this.opts, (o: ThemeOpt) => {
        Row() {
          Text(o.label).fontSize(16).layoutWeight(1)
          if (this.pref === o.mode) {
            Text('✓').fontSize(16).fontColor($r('app.color.brand_text'))
          }
        }
        .width('100%')
        .padding({ top: 13, bottom: 13 })
        .alignItems(VerticalAlign.Center)
        .border({ width: { bottom: 0.5 }, color: $r('app.color.divider') })
        .onClick(() => {
          this.apply(o.mode);
        })
      }, (o: ThemeOpt) => o.mode.toString())
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
        .fontColor($r('app.color.text_primary'))
        .width('100%')
        .textAlign(TextAlign.Center)
        .padding({ top: 4, bottom: 4 })

      Slider({ value: this.idx, min: 0, max: 4, step: 1, style: SliderStyle.OutSet })
        .width('100%')
        .blockColor($r('app.color.on_brand'))
        .selectedColor($r('app.color.brand'))
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
            .fontColor(i === this.idx ? $r('app.color.brand_text') : $r('app.color.text_tertiary'))
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

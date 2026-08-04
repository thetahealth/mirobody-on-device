import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import hilog from '@ohos.hilog';
import UIAbility from '@ohos.app.ability.UIAbility';
import Want from '@ohos.app.ability.Want';
import window from '@ohos.window';
import { Configuration } from '@ohos.app.ability.Configuration';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onCreate');
    // App-wide font scale from the settings menu; persisted across launches.
    PersistentStorage.persistProp('fontScale', 1);
    // Theme override from the settings menu. Stored as the raw ColorMode value so there is no
    // mapping table: NOT_SET(-1) = follow the system, which is also the first-run default.
    PersistentStorage.persistProp('colorModePref', ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    this.applyStoredColorMode();
  }

  /**
   * Re-apply the user's theme override on launch. setColorMode only affects the running
   * process, so without this the app would fall back to the system theme every cold start.
   */
  private applyStoredColorMode(): void {
    const stored = AppStorage.get<number>('colorModePref');
    const mode: ConfigurationConstant.ColorMode = stored === undefined
      ? ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET
      : stored as ConfigurationConstant.ColorMode;
    if (mode === ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET) {
      this.publishColorMode(this.context.config.colorMode);
      return;
    }
    this.context.getApplicationContext().setColorMode(mode);
    // Publish from the override rather than from config: the configuration update it triggers
    // is async, and the render host would otherwise theme its first formula for the old mode.
    this.publishColorMode(mode);
  }

  /**
   * Colors themselves swap automatically (resources/dark/element/color.json), but the
   * offscreen render host can't see resources: MathJax fills glyphs with `currentColor` and
   * ECharts bakes a background into the PNG, so both need the mode passed in explicitly and
   * both must re-render when it flips. Publishing it to AppStorage is what makes RichMessage
   * re-ask RenderHost under a mode-specific cache key.
   */
  onConfigurationUpdate(newConfig: Configuration): void {
    this.publishColorMode(newConfig.colorMode);
  }

  private publishColorMode(mode: ConfigurationConstant.ColorMode | undefined): void {
    AppStorage.setOrCreate<boolean>('darkMode', mode === ConfigurationConstant.ColorMode.COLOR_MODE_DARK);
  }

  onDestroy(): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onWindowStageCreate');

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(0x0000, 'mirobody', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err) ?? '');
        return;
      }
      hilog.info(0x0000, 'mirobody', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    hilog.info(0x0000, 'mirobody', '%{public}s', 'Ability onBackground');
  }
}

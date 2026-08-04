/**
 * Left navigation drawer — the app's ONLY menu, following htdoc's `history.js`.
 *
 * Layout, same bands as the web client: a header (back + "Menu"), the "New chat" /
 * Incognito button row, the session list as the ONLY scrolling area in the middle,
 * and a pinned footer holding the app-settings group.
 *
 * The settings group used to be a top-right gear, as on the web until it folded the
 * same menu in here: two menu affordances on one bar (a hamburger AND a gear) made
 * "where does this setting live" a coin flip. One hamburger now opens everything.
 *
 * Two departures from htdoc remain, both because harmony has no server and no account
 * (it talks straight to each provider's baseUrl):
 *   - no Health & data group and no account rows. Care circle / EHR / devices / sign
 *     out are all server screens harmony does not have.
 *   - no Backend row. BYOK has no single backend; per-provider base_url is set in the
 *     cloud-service manager instead.
 *
 * This is an in-tree overlay rather than a bindSheet/bindContentCover for the same
 * reason the model picker is (see Index.ets): an in-tree layer re-renders reactively
 * when its data changes, so deleting a row updates the list immediately.
 */
import { SessionMeta } from '../core/SessionStore';
import { i18n } from '@kit.LocalizationKit';
import ConfigurationConstant from '@ohos.app.ability.ConfigurationConstant';

@Component
export struct HistoryDrawer {
  /** Newest-first, loaded by the host each time the drawer opens. */
  @Prop sessions: SessionMeta[];
  /** The conversation on screen, highlighted in the list. '' when it is unsaved. */
  @Prop activeId: string;

  /** Whether the host is in incognito, so the toggle can show as active. */
  @Prop incognito: boolean;
  /** True while a turn streams: the incognito toggle is inert then, as on the web,
   *  because swapping the conversation out from under an in-flight reply would
   *  leave its callbacks appending onto a session that is no longer on screen. */
  @Prop busy: boolean;

  onNewChat: () => void = () => {};
  onIncognito: () => void = () => {};
  onPick: (id: string) => void = () => {};
  onDelete: (id: string) => void = () => {};
  onClose: () => void = () => {};

  /** The settings rows open dialogs the host owns (Index.ets holds every
   *  CustomDialogController), so each row is a callback rather than a controller here. */
  onLanguage: () => void = () => {};
  onFontSize: () => void = () => {};
  onTheme: () => void = () => {};
  onNativeProbe: () => void = () => {};

  /** Live settings values, shown as each row's trailing hint so the current choice is
   *  readable without opening the dialog. Both are the same AppStorage keys the
   *  dialogs write, so a change lands on the row as soon as it is applied. */
  @StorageLink('fontScale') private fontScale: number = 1;
  // NOT_SET, not 0 -- 0 IS COLOR_MODE_DARK, so a plain zero default would read back as
  // "Dark" on the one path where AppStorage hasn't been seeded yet.
  @StorageLink('colorModePref') private colorModePref: number =
    ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET;

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  /** The language's own endonym, matching how LanguageDialog lists it. */
  private languageHint(): string {
    let tag = '';
    try {
      tag = i18n.System.getAppPreferredLanguage();
    } catch (err) {
      tag = '';
    }
    if (tag.startsWith('zh')) {
      return '简体中文';
    }
    if (tag.startsWith('en')) {
      return 'English';
    }
    return tag;
  }

  /** Nearest of FontSizeDialog's five tiers to the stored scale — the same
   *  nearest-match the dialog itself uses to place its slider. */
  private fontHint(): string {
    const scales: number[] = [0.8, 0.9, 1.0, 1.15, 1.3];
    const labels: Resource[] = [
      $r('app.string.font_smaller'),
      $r('app.string.font_small'),
      $r('app.string.font_standard'),
      $r('app.string.font_large'),
      $r('app.string.font_larger')
    ];
    let best = 2;
    let bestD = 999;
    for (let i = 0; i < scales.length; i++) {
      const d = Math.abs(scales[i] - this.fontScale);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    return this.rs(labels[best]);
  }

  private themeHint(): string {
    if (this.colorModePref === ConfigurationConstant.ColorMode.COLOR_MODE_LIGHT) {
      return this.rs($r('app.string.theme_light'));
    }
    if (this.colorModePref === ConfigurationConstant.ColorMode.COLOR_MODE_DARK) {
      return this.rs($r('app.string.theme_dark'));
    }
    return this.rs($r('app.string.theme_system'));
  }

  /**
   * One footer row: label on the leading edge, current value on the trailing edge.
   * Tighter than a session row (the whole group is pinned and must fit without a
   * scroll of its own), but tall enough to stay a comfortable tap target.
   */
  @Builder
  private navRow(label: ResourceStr, hint: string, tap: () => void) {
    Row() {
      Text(label)
        .fontSize(15)
        .fontColor($r('app.color.text_primary'))
        .layoutWeight(1)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .textAlign(TextAlign.Start)
      if (hint.length > 0) {
        Text(hint)
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ left: 12 })
      }
    }
    .width('100%')
    .height(44)
    .padding({ left: 16, right: 16 })
    .alignItems(VerticalAlign.Center)
    .onClick(tap)
  }

  /**
   * Relative for the recent past, absolute beyond a week. A bare clock time on a
   * three-week-old session reads as "today" at a glance, which is worse than no
   * timestamp; a bare date on something from ten minutes ago is equally unhelpful.
   */
  private stamp(ts: number): string {
    const mins = Math.floor((new Date().getTime() - ts) / 60000);
    if (mins < 1) {
      return this.rs($r('app.string.drawer_just_now'));
    }
    if (mins < 60) {
      return this.rs($r('app.string.drawer_mins_ago'), mins);
    }
    const hours = Math.floor(mins / 60);
    if (hours < 24) {
      return this.rs($r('app.string.drawer_hours_ago'), hours);
    }
    const days = Math.floor(hours / 24);
    if (days < 7) {
      return this.rs($r('app.string.drawer_days_ago'), days);
    }
    const d = new Date(ts);
    return this.rs($r('app.string.drawer_date'), d.getMonth() + 1, d.getDate());
  }

  @Builder
  private row(s: SessionMeta) {
    Row() {
      Column() {
        Text(s.title.length > 0 ? s.title : this.rs($r('app.string.drawer_untitled')))
          .fontSize(15)
          .fontColor($r('app.color.text_primary'))
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .width('100%')
          .textAlign(TextAlign.Start)
        Text(this.stamp(s.updatedAt) + ' · ' + this.rs($r('app.string.drawer_msg_count'), s.count))
          .fontSize(12)
          .fontColor($r('app.color.text_tertiary'))
          .margin({ top: 4 })
          .width('100%')
          .textAlign(TextAlign.Start)
      }
      .layoutWeight(1)
      .alignItems(HorizontalAlign.Start)

      // Per-row delete, as on the web. The host asks for confirmation -- a mis-tap
      // here would otherwise destroy a conversation with no undo.
      SymbolGlyph($r('sys.symbol.trash'))
        .fontSize(18)
        .fontColor([$r('app.color.text_tertiary')])
        .margin({ left: 8 })
        .onClick(() => {
          this.onDelete(s.id);
        })
    }
    .width('100%')
    .padding({ left: 16, right: 16, top: 12, bottom: 12 })
    // The open conversation is marked so "which one am I in" is answerable without
    // closing the drawer and reading the transcript.
    .backgroundColor(s.id === this.activeId ? $r('app.color.bg_bubble') : Color.Transparent)
    .borderRadius(10)
    .onClick(() => {
      this.onPick(s.id);
    })
  }

  build() {
    // Row + FlexAlign.Start anchors the panel to the inline-start edge, so it lands on
    // the right side under an RTL locale without a second code path.
    //
    // This Row IS the scrim: it carries the dim fill and the tap-to-dismiss. An earlier
    // version left those on a separate sibling layer in Index.ets, which never fired --
    // a full-width Row painted on top of it swallowed every tap on the blank area, so
    // the only way out of the drawer was to pick something. One node owning both the
    // dim and the dismiss is what makes that unable to drift apart again.
    Row() {
      Column() {
        // --- pinned top: header (back + title), then the New chat / Incognito row ---
        // The back arrow is the same dismissal the scrim tap gives, spelled out: with
        // the settings group living down in the footer this panel is a screen people
        // read rather than a list they pick from, and a screen needs a visible way out.
        Row() {
          SymbolGlyph($r('sys.symbol.chevron_left'))
            .fontSize(22)
            .fontColor([$r('app.color.text_secondary')])
            .onClick(() => {
              this.onClose();
            })
          Text($r('app.string.drawer_menu_title'))
            .fontSize(16)
            .fontWeight(FontWeight.Medium)
            .fontColor($r('app.color.text_primary'))
            .margin({ left: 8 })
        }
        .width('100%')
        .height(52)
        .padding({ left: 12, right: 16 })
        .alignItems(VerticalAlign.Center)

        Row() {
          Row() {
            SymbolGlyph($r('sys.symbol.plus_circle'))
              .fontSize(20)
              .fontColor([$r('app.color.brand_text')])
            Text($r('app.string.drawer_new_chat'))
              .fontSize(15)
              .fontColor($r('app.color.brand_text'))
              .margin({ left: 8 })
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .layoutWeight(1)
          .padding({ top: 12, bottom: 12, left: 12, right: 8 })
          .onClick(() => {
            this.onNewChat();
          })

          // Active state is a filled pill, not just a tint: "am I being recorded"
          // has to be answerable at a glance, and a colour shift alone is easy to
          // miss (and invisible to anyone who cannot distinguish the two hues).
          Row() {
            SymbolGlyph($r('sys.symbol.eye_slash'))
              .fontSize(18)
              .fontColor([this.incognito ? $r('app.color.on_brand') : $r('app.color.text_secondary')])
            Text($r('app.string.drawer_incognito'))
              .fontSize(14)
              .fontColor(this.incognito ? $r('app.color.on_brand') : $r('app.color.text_secondary'))
              .margin({ left: 6 })
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }
          .padding({ top: 8, bottom: 8, left: 10, right: 10 })
          .backgroundColor(this.incognito ? $r('app.color.brand') : Color.Transparent)
          .borderRadius(16)
          .opacity(this.busy ? 0.4 : 1)
          .onClick(() => {
            if (this.busy) {
              return;
            }
            this.onIncognito();
          })
        }
        .width('100%')
        .padding({ left: 6, right: 10, top: 6, bottom: 6 })

        Divider().strokeWidth(0.5).color($r('app.color.border'))

        // --- the only scrolling band: sessions ---
        if (this.sessions.length === 0) {
          Column() {
            Text($r('app.string.drawer_empty'))
              .fontSize(14)
              .fontColor($r('app.color.text_tertiary'))
              .textAlign(TextAlign.Center)
          }
          .layoutWeight(1)
          .width('100%')
          .justifyContent(FlexAlign.Center)
          .padding(24)
        } else {
          List() {
            ForEach(this.sessions, (s: SessionMeta) => {
              ListItem() {
                this.row(s)
              }
            // Key includes updatedAt and count: keying on id alone would make ArkUI
            // skip a row whose title or message count just changed (the same
            // content-extent trap as the streaming message list).
            }, (s: SessionMeta) => s.id + '#' + s.updatedAt.toString() + '#' + s.count.toString()
                 + (s.id === this.activeId ? '#a' : ''))
          }
          .layoutWeight(1)
          .width('100%')
          .padding({ left: 6, right: 6, top: 6 })
          .edgeEffect(EdgeEffect.Spring)
          .scrollBar(BarState.Auto)
        }

        // --- pinned footer: the app-settings group, then the storage note ---
        // Pinned (no layoutWeight) so only the session list above it scrolls; the rows
        // are compact enough that the group fits without a scroll of its own.
        Divider().strokeWidth(0.5).color($r('app.color.border'))

        this.navRow($r('app.string.settings_language'), this.languageHint(), () => {
          this.onClose();
          this.onLanguage();
        })
        this.navRow($r('app.string.settings_font_size'), this.fontHint(), () => {
          this.onClose();
          this.onFontSize();
        })
        this.navRow($r('app.string.settings_theme'), this.themeHint(), () => {
          this.onClose();
          this.onTheme();
        })
        // Temporary: step-0 native bridge probe. Remove with pages/NativeProbe.ets
        // once the real native chat lane is wired.
        this.navRow('Native probe', '', () => {
          this.onClose();
          this.onNativeProbe();
        })

        Divider().strokeWidth(0.5).color($r('app.color.border'))
        Row() {
          Text($r('app.string.drawer_local_only'))
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 10, bottom: 10 })
      }
      // 85% mirrors htdoc's min(85vw, 360px); it keeps a strip of the chat visible so
      // the drawer reads as a layer over the conversation, not a separate screen.
      .width('85%')
      .constraintSize({ maxWidth: 360 })
      .height('100%')
      .backgroundColor($r('app.color.bg_surface'))
      .shadow({ radius: 16, color: $r('app.color.shadow'), offsetX: 2, offsetY: 0 })
      // Swallow taps that land on the panel so they do not reach the scrim's
      // handler below and close the drawer the user is trying to use.
      .onClick(() => {})
    }
    .width('100%')
    .height('100%')
    .justifyContent(FlexAlign.Start)
    .backgroundColor($r('app.color.scrim'))
    .onClick(() => {
      this.onClose();
    })
  }
}

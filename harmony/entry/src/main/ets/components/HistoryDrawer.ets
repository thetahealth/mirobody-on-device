/**
 * Left navigation drawer — the app's nav home, following htdoc's `history.js`.
 *
 * Layout, same three bands as the web client: "New chat" pinned at the top, the
 * session list as the ONLY scrolling area in the middle, and a pinned footer.
 *
 * Two deliberate departures from htdoc, both because harmony has no server and no
 * account (it talks straight to each provider's baseUrl):
 *   - no Incognito toggle. On the web that means "don't persist to my account";
 *     with local-only history and no account there is nothing for it to mean.
 *   - no Health & data group and no Sign out. Care circle / EHR / devices are all
 *     server screens harmony does not have.
 * App settings stay in the top-bar gear, NOT here — that is htdoc's rule too, so the
 * two clients keep settings in the same place.
 *
 * This is an in-tree overlay rather than a bindSheet/bindContentCover for the same
 * reason the model picker is (see Index.ets): an in-tree layer re-renders reactively
 * when its data changes, so deleting a row updates the list immediately.
 */
import { SessionMeta } from '../core/SessionStore';

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

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
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
        // --- pinned top: the New chat / Incognito button row ---
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

        // --- pinned footer ---
        Divider().strokeWidth(0.5).color($r('app.color.border'))
        Row() {
          Text($r('app.string.drawer_local_only'))
            .fontSize(11)
            .fontColor($r('app.color.text_tertiary'))
            .layoutWeight(1)
        }
        .width('100%')
        .padding({ left: 16, right: 16, top: 12, bottom: 12 })
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

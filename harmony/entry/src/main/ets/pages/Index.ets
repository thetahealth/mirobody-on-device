import { common } from '@kit.AbilityKit';
import { promptAction, display, router } from '@kit.ArkUI';
import { ChatMessage, MessagePart, StreamCallbacks, Lane, RouteDecision, Canceler } from '../model/Chat';
import { RichMessage } from '../components/RichMessage';
import { RenderHost } from '../core/RenderHost';
import { ChatEngine, TurnContext } from '../core/ChatEngine';
import { findProvider, BUILTIN_PROVIDERS } from '../model/Providers';
import { SecureKeyStore } from '../core/SecureKeyStore';
import { ConfigStore, ActiveSelection, AddedModel } from '../core/ConfigStore';
import { ProviderManagerDialog } from '../components/ProviderManagerDialog';
import { AddModelDialog } from '../components/AddModelDialog';
import { LanguageDialog, FontSizeDialog, ThemeDialog } from '../components/AppSettingsDialogs';
import { HealthDataDialog } from '../components/HealthDataDialog';
import { HealthRepository } from '../core/HealthRepository';
import { LocalModelStore, LocalModel, LOCAL_PROVIDER_ID } from '../core/LocalModelStore';
import { HistoryDrawer } from '../components/HistoryDrawer';
import { SessionStore, SessionMeta } from '../core/SessionStore';
import { FONT, ICON } from '../model/Typography';

// User-added models grouped under one provider header in the dropdown.
interface ProviderGroup {
  providerId: string;
  label: string;
  models: AddedModel[];
}

@Entry
@Component
struct Index {
  @State private history: ChatMessage[] = [];
  @State private draft: string = '';
  @State private streaming: string = '';
  // The in-flight turn's chain-of-thought (reasoning models). Rendered live by
  // RichMessage while the answer hasn't started, folded once it has.
  @State private streamingThinking: string = '';
  @State private busy: boolean = false;
  @State private laneNotice: string = '';
  // Active conversation model (key lives per-provider in the secure store).
  @State private activeProviderId: string = '';
  @State private activeModel: string = '';
  @State private activeHasKey: boolean = false;
  // Whether ANY cloud service (provider key) is configured — gates the "配置模型" menu entry,
  // which is meaningless with no key behind it (you'd only see the "add a key first" screen).
  @State private hasAnyKey: boolean = false;
  // Everything the picker offers: cloud models the user added, plus on-device models
  // from LocalModelStore under the LOCAL_PROVIDER_ID sentinel. Carrying local models
  // in the same AddedModel shape is what keeps groups()/selectModel()/pickerLabel()
  // free of lane special-cases.
  @State private addedModels: AddedModel[] = [];
  // Local model id -> absolute path, so a turn can snapshot the path it will mmap.
  private localPaths: Map<string, string> = new Map();
  // Snapshot of added-model keys taken when the add dialog opens, so on close we can detect
  // what was newly added this session and auto-select the last one.
  private addedSnapshot: Set<string> = new Set<string>();
  @State private showPicker: boolean = false;

  // Left drawer (components/HistoryDrawer.ets). `sessionId` is the row the on-screen
  // conversation belongs to; it is minted on first send rather than at startup so an
  // app that is opened and closed without a message leaves nothing behind.
  @State private showDrawer: boolean = false;
  // Last health sync (epoch ms, 0 = never), read when the drawer opens so its
  // health row can carry the same kind of hint the settings rows do.
  @State private healthLastSync: number = 0;
  @State private sessions: SessionMeta[] = [];
  @State private sessionId: string = '';
  /**
   * Incognito, following htdoc's toggleIncognito: it SWAPS the session rather than
   * merely suppressing writes. Entering stashes the real conversation and starts a
   * blank ephemeral one; leaving restores the stash and discards the ephemeral turns.
   * A plain "don't save" flag would be worse -- the private turns would stay mixed into
   * the visible transcript, so the next non-private message would save them too.
   */
  @State private incognito: boolean = false;
  private stashHistory: ChatMessage[] = [];
  private stashSessionId: string = '';
  // Picker list cap — computed from screen height so it only scrolls when it truly won't fit.
  @State private pickerMaxHeight: number = 320;
  private scroller: Scroller = new Scroller();
  /**
   * Sticky-bottom streaming: follow new tokens while the view is at the bottom, and
   * stop the moment the user scrolls away so reading back never fights the stream.
   * Re-arms by itself when they return to the bottom.
   */
  @State private stickToBottom: boolean = true;
  // Cancel handle for the in-flight turn (lets the user stop a slow/stuck response).
  private canceler: Canceler = { cancel: () => {}, stopped: false };
  // Monotonic turn counter. Every stream callback captures the id of the turn that created it
  // and no-ops unless it still matches, so a stopped or superseded turn can never touch the
  // UI again. A shared "cancelled" flag can't do this: the next send resets it, which lets the
  // old stream's late onDone/onError commit or wipe the NEW turn's text.
  private turnSeq: number = 0;
  // App-wide font scale from the settings menu (persisted; see EntryAbility).
  @StorageLink('fontScale') private fontScale: number = 1;

  private langController: CustomDialogController = new CustomDialogController({
    builder: LanguageDialog(),
    autoCancel: true,
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private fontController: CustomDialogController = new CustomDialogController({
    builder: FontSizeDialog(),
    autoCancel: true,
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private themeController: CustomDialogController = new CustomDialogController({
    builder: ThemeDialog(),
    autoCancel: true,
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private healthController: CustomDialogController = new CustomDialogController({
    builder: HealthDataDialog({
      onDone: () => {
        this.refreshHealthHint();
      }
    }),
    autoCancel: true,
    // Dismissed by backdrop / back key: refresh the drawer's hint too, since a
    // sync may well have happened before the user tapped outside.
    cancel: () => {
      this.refreshHealthHint();
    },
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  // The settings dialogs above are opened from the nav drawer's footer rows -- there is
  // no gear menu any more. The controllers stay here rather than moving into the drawer
  // because a CustomDialogController owned by the drawer would be torn down with it the
  // moment a row dismissed the drawer to show its dialog.

  private mgrController: CustomDialogController = new CustomDialogController({
    builder: ProviderManagerDialog({
      onDone: () => {
        this.refresh();
      },
      // After a key is saved, jump straight to configuring that service's models.
      onConfigureModels: (_providerId: string) => {
        this.openAddModelsAfterSave();
      }
    }),
    autoCancel: true,
    // Fired when dismissed by tapping the backdrop / back key — also refresh then.
    cancel: () => {
      this.refresh();
    },
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private addController: CustomDialogController = new CustomDialogController({
    builder: AddModelDialog({
      onDone: () => {
        this.afterAddDialog();
      }
    }),
    autoCancel: true,
    cancel: () => {
      this.afterAddDialog();
    },
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private ctx(): common.UIAbilityContext {
    return getContext(this) as common.UIAbilityContext;
  }

  private rs(res: Resource, ...a: (string | number)[]): string {
    return getContext(this).resourceManager.getStringSync(res.id, ...a);
  }

  async aboutToAppear(): Promise<void> {
    RenderHost.init(this.ctx().filesDir);
    this.computePickerHeight();
    await this.refresh();
  }

  // Cap the picker list at ~85% of screen height — it's a transient dropdown, so it may be
  // tall; it only scrolls when it genuinely can't fit.
  private computePickerHeight(): void {
    try {
      const d = display.getDefaultDisplaySync();
      if (d.densityPixels > 0) {
        this.pickerMaxHeight = Math.floor((d.height / d.densityPixels) * 0.85);
      }
    } catch (err) {
      // keep the default fallback
    }
  }

  // Reload the added-model list + active selection. Called on open and after either dialog.
  private async refresh(): Promise<void> {
    const cloud: AddedModel[] = await ConfigStore.getAddedModels(this.ctx());
    const locals: LocalModel[] = await LocalModelStore.list(this.ctx());

    const paths: Map<string, string> = new Map();
    const merged: AddedModel[] = [];
    for (const a of cloud) { merged.push(a); }
    for (const m of locals) {
      paths.set(m.id, m.path);
      merged.push({ providerId: LOCAL_PROVIDER_ID, id: m.id, displayName: m.displayName });
    }
    this.localPaths = paths;
    this.addedModels = merged;

    // A local model whose file moved is dropped during activation; say so, or it
    // looks like the app forgot it and there is no hint that re-importing fixes it.
    const gone: string[] = LocalModelStore.takeDropped();
    if (gone.length > 0) {
      promptAction.showToast({
        message: this.rs($r('app.string.chat_local_model_missing'), gone.join('、')),
        duration: 4000
      });
    }
    this.hasAnyKey = await this.anyKeyConfigured();

    const sel: ActiveSelection = await ConfigStore.load(this.ctx());
    this.activeProviderId = sel.providerId;
    // Keep the selection only if that model is still offered. A cloud model also needs
    // its provider's key to still be there (it may have been deleted, and legacy data
    // may hold a model whose key is long gone); a local model needs no key at all.
    const isLocal = sel.providerId === LOCAL_PROVIDER_ID;
    const hasKey = isLocal ? false : await SecureKeyStore.hasKey(sel.providerId);
    if ((isLocal || hasKey) && this.isAddedModel(sel.providerId, sel.model)) {
      this.activeModel = sel.model;
      this.activeHasKey = hasKey;
    } else {
      this.activeModel = '';
      this.activeHasKey = false;
    }
  }

  private modelKey(providerId: string, id: string): string {
    return providerId + '::' + id;
  }

  // True if at least one provider has a saved key (i.e. any cloud service is configured).
  private async anyKeyConfigured(): Promise<boolean> {
    for (const p of BUILTIN_PROVIDERS) {
      if (await SecureKeyStore.hasKey(p.id)) {
        return true;
      }
    }
    return false;
  }

  // Right after a key is validated & saved in the provider manager: reload state, then open
  // the add-model dialog. That dialog defaults to the most-recently-saved provider, so it
  // lands on the service the user just configured.
  private async openAddModelsAfterSave(): Promise<void> {
    await this.refresh();
    this.snapshotAdded();
    this.addController.open();
  }

  // Record which models are already added, right before opening the add dialog.
  private snapshotAdded(): void {
    const s = new Set<string>();
    for (const a of this.addedModels) {
      s.add(this.modelKey(a.providerId, a.id));
    }
    this.addedSnapshot = s;
  }

  // On add-dialog close: reload state, then make the last model added this session the active
  // one. Additions are appended in order, so the last still-present new entry is the latest
  // add (any added-then-removed model is already gone from the list). Selecting nothing new
  // leaves refresh()'s keep-if-present behavior intact.
  private async afterAddDialog(): Promise<void> {
    const prev = this.addedSnapshot;
    await this.refresh();
    let lastNew: AddedModel | undefined = undefined;
    for (const a of this.addedModels) {
      if (!prev.has(this.modelKey(a.providerId, a.id))) {
        lastNew = a;
      }
    }
    if (lastNew !== undefined) {
      await this.selectModel(lastNew.providerId, lastNew.id);
    }
  }

  private isAddedModel(providerId: string, model: string): boolean {
    for (const a of this.addedModels) {
      if (a.providerId === providerId && a.id === model) {
        return true;
      }
    }
    return false;
  }

  private async selectModel(providerId: string, model: string): Promise<void> {
    this.activeProviderId = providerId;
    this.activeModel = model;
    this.activeHasKey = providerId === LOCAL_PROVIDER_ID
      ? false
      : await SecureKeyStore.hasKey(providerId);
    await ConfigStore.save(this.ctx(), { providerId: providerId, model: model });
  }

  // Remove a model straight from the picker (keeps the popup open to remove more). If it was
  // the active one, clear the selection back to "选择模型".
  private async removeModel(m: AddedModel): Promise<void> {
    const wasActive = this.activeProviderId === m.providerId && this.activeModel === m.id;
    if (m.providerId === LOCAL_PROVIDER_ID) {
      // Forgets the reference only — the user's model file is never deleted.
      await LocalModelStore.remove(this.ctx(), m.id);
    } else {
      await ConfigStore.removeAddedModel(this.ctx(), m.providerId, m.id);
    }
    await this.refresh();
    if (wasActive) {
      this.activeModel = '';
    }
    promptAction.showToast({ message: this.rs($r('app.string.toast_removed'), m.displayName), duration: 1500 });
  }

  /** Pick a .gguf and make it the active model, so the next turn runs on-device. */
  private async importLocalModel(): Promise<void> {
    const m: LocalModel | undefined = await LocalModelStore.importModel(this.ctx());
    if (m === undefined) {
      return;   // cancelled, or the file turned out not to be reachable
    }
    await this.refresh();
    await this.selectModel(LOCAL_PROVIDER_ID, m.id);
    promptAction.showToast({
      message: this.rs($r('app.string.toast_added'), m.displayName), duration: 1800
    });
  }

  private pickerLabel(): string {
    // Only activeModel is checked: refresh() already refuses to set it unless the
    // selection is usable — a cloud model with its key, a local model outright. An
    // extra activeHasKey test here used to blank the label for every local model,
    // since activeHasKey is deliberately false for a lane that needs no key.
    if (this.activeModel.length === 0) {
      return this.rs($r('app.string.chat_pick_model'));
    }
    if (this.activeProviderId === LOCAL_PROVIDER_ID) {
      // No prefix. For a cloud model the prefix names WHICH service, which is real
      // information; "on-device" is only a category, the picker already groups under
      // that header, and the .gguf name says it anyway — so it would just be a second
      // copy of the same word eating the pill's one line of width.
      return this.activeModel;
    }
    const p = findProvider(this.activeProviderId);
    const label = p !== undefined ? p.label : this.activeProviderId;
    return label + ' · ' + this.activeModel;
  }

  private groups(): ProviderGroup[] {
    const byP: Map<string, AddedModel[]> = new Map();
    for (const a of this.addedModels) {
      const arr = byP.get(a.providerId);
      if (arr === undefined) {
        byP.set(a.providerId, [a]);
      } else {
        arr.push(a);
      }
    }
    const out: ProviderGroup[] = [];
    byP.forEach((models: AddedModel[], pid: string) => {
      if (pid === LOCAL_PROVIDER_ID) {
        out.push({ providerId: pid, label: this.rs($r('app.string.chat_local_group')), models: models });
        return;
      }
      const p = findProvider(pid);
      out.push({ providerId: pid, label: p !== undefined ? p.label : pid, models: models });
    });
    out.sort((a: ProviderGroup, b: ProviderGroup) => a.label.localeCompare(b.label));
    return out;
  }

  // Conversation-time picker (custom popup): user-added models grouped by provider — tap the
  // name to select, tap ✕ to delete it right here — plus the two management entries.
  @Builder
  private pickerPanel() {
    Column() {
      Scroll() {
        Column() {
          ForEach(this.groups(), (g: ProviderGroup) => {
            Text(g.label)
              .fontSize(FONT.caption)
              .fontColor($r('app.color.text_tertiary'))
              .width('100%')
              .padding({ left: 16, right: 16, top: 10, bottom: 4 })
            ForEach(g.models, (m: AddedModel) => {
              Row() {
                Text(m.displayName)
                  .fontSize(FONT.row)
                  .layoutWeight(1)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .onClick(() => {
                    this.showPicker = false;
                    this.selectModel(m.providerId, m.id);
                  })
                Button({ type: ButtonType.Circle }) {
                  Text('－').fontSize(ICON.md).fontColor($r('app.color.on_brand')).fontWeight(FontWeight.Bold)
                }
                .width(28)
                .height(28)
                .margin({ left: 12 })
                .backgroundColor($r('app.color.danger'))
                .onClick(() => {
                  this.removeModel(m);
                })
              }
              .width('100%')
              .padding({ left: 16, right: 24, top: 14, bottom: 14 })
              .alignItems(VerticalAlign.Center)

              Divider()
                .strokeWidth(0.5)
                .color($r('app.color.divider'))
                .margin({ left: 12, right: 12 })
            }, (m: AddedModel) => m.providerId + '::' + m.id)
            // Key includes the group's model ids so removing one re-renders this group
            // (ArkUI skips subtrees whose ForEach key is unchanged).
          }, (g: ProviderGroup) => g.providerId + '#' + g.models.map((x: AddedModel) => x.id).join(','))
        }
        .width('100%')
      }
      .constraintSize({ maxHeight: this.pickerMaxHeight })
      .width('100%')
      .scrollBar(BarState.Auto)

      Divider()
        .strokeWidth(0.5)
        .color($r('app.color.border'))
        .margin({ top: 6, bottom: 2 })
      if (this.hasAnyKey) {
        Row() {
          Text($r('app.string.chat_menu_config_model')).fontSize(FONT.row).fontColor($r('app.color.brand_text'))
        }
        .width('100%')
        .padding(16)
        .onClick(() => {
          this.showPicker = false;
          this.snapshotAdded();
          this.addController.open();
        })
      }
      Row() {
        Text($r('app.string.chat_menu_config_cloud')).fontSize(FONT.row).fontColor($r('app.color.brand_text'))
      }
      .width('100%')
      .padding(16)
      .onClick(() => {
        this.showPicker = false;
        this.mgrController.open();
      })
      // No key needed for this one, so it is never gated.
      Row() {
        Text($r('app.string.chat_menu_import_local')).fontSize(FONT.row).fontColor($r('app.color.brand_text'))
      }
      .width('100%')
      .padding(16)
      .onClick(() => {
        this.showPicker = false;
        this.importLocalModel();
      })
    }
    .width('90%')
    .padding({ top: 8, bottom: 8, left: 6, right: 6 })
  }

  // Called on every delta. A no-op unless the view is still following the stream —
  // and harmless when the content does not overflow, since scrollEdge does nothing.
  private scrollToEnd(): void {
    if (!this.stickToBottom) {
      return;
    }
    this.scroller.scrollEdge(Edge.Bottom);
  }

  /**
   * Re-evaluate whether to keep following, after a USER-driven scroll.
   *
   * isAtEnd() is the judge rather than the scroll delta's sign: it needs no guess
   * about which direction is "up", and it gets the edge cases right for free — an
   * overscroll bounce settles back at the end and stays stuck, while a drag that
   * lands anywhere else unsticks.
   */
  private syncStick(): void {
    this.stickToBottom = this.scroller.isAtEnd();
  }

  // The finished turn as a history message. `content` carries ONLY the answer
  // text (it goes back to the model as context — replaying chain-of-thought
  // would bloat and skew later turns); the thinking survives in parts, for
  // display alone.
  //--------------------------------------------------------------------------
  // Left drawer / session history
  //--------------------------------------------------------------------------

  /**
   * Write the on-screen conversation to its session row. Called after every change to
   * `history` (user turn appended, assistant turn finished, partial kept on stop) rather
   * than only at the end of a turn, so killing the app mid-reply still leaves the
   * question saved.
   *
   * Minting the id here, on the first save, is what keeps an opened-and-abandoned app
   * from creating an empty row.
   */
  private persist(): void {
    // The whole point of incognito: nothing reaches disk, including the index row.
    if (this.incognito || this.history.length === 0) {
      return;
    }
    if (this.sessionId.length === 0) {
      this.sessionId = SessionStore.newId();
    }
    // Deliberately not awaited: a turn must not wait on disk. A failed write costs the
    // history row, not the conversation on screen.
    SessionStore.save(this.ctx(), this.sessionId, this.history);
  }

  private async openDrawer(): Promise<void> {
    this.sessions = await SessionStore.list(this.ctx());
    await this.refreshHealthHint();
    this.showDrawer = true;
  }

  /** Read the last health-sync time for the drawer's row hint (0 = never). */
  private async refreshHealthHint(): Promise<void> {
    this.healthLastSync = await HealthRepository.lastSync(this.ctx());
  }

  /**
   * Start a fresh conversation. The outgoing one is already persisted (every turn saves),
   * so this only has to clear the view -- and must drop `sessionId`, or the next message
   * would overwrite the conversation the user just left.
   */
  /**
   * Enter or leave incognito. Inert while a turn streams (the drawer greys the control
   * out too): swapping the conversation mid-reply would let that turn's callbacks append
   * onto a session no longer on screen.
   */
  private toggleIncognito(): void {
    if (this.busy) {
      return;
    }
    if (!this.incognito) {
      this.stashHistory = this.history;
      this.stashSessionId = this.sessionId;
      this.history = [];
      this.sessionId = '';
      this.incognito = true;
    } else {
      this.history = this.stashHistory;
      this.sessionId = this.stashSessionId;
      this.stashHistory = [];
      this.stashSessionId = '';
      this.incognito = false;
    }
    this.streaming = '';
    this.streamingThinking = '';
    this.laneNotice = '';
    this.showDrawer = false;
    this.stickToBottom = true;
    this.scrollToEnd();
  }

  // Note this deliberately does NOT clear `incognito`: starting another chat while
  // private must not silently resume recording. Leaving is always an explicit tap.
  private newChat(): void {
    if (this.busy) {
      this.onStop();
    }
    this.history = [];
    this.streaming = '';
    this.streamingThinking = '';
    this.laneNotice = '';
    this.sessionId = '';
    this.showDrawer = false;
    this.stickToBottom = true;
  }

  private async pickSession(id: string): Promise<void> {
    if (id === this.sessionId) {
      this.showDrawer = false;
      return;
    }
    // Stop any in-flight turn FIRST: its callbacks would otherwise append their reply
    // onto the conversation we are about to load. onStop bumps turnSeq, which retires
    // them permanently.
    if (this.busy) {
      this.onStop();
    }
    const msgs = await SessionStore.messages(this.ctx(), id);
    this.history = msgs;
    this.streaming = '';
    this.streamingThinking = '';
    this.laneNotice = '';
    this.sessionId = id;
    this.showDrawer = false;
    this.stickToBottom = true;
    this.scrollToEnd();
  }

  /** Per-row delete. Confirmed first — there is no undo. */
  private async confirmDelete(id: string): Promise<void> {
    const answer = await promptAction.showDialog({
      title: this.rs($r('app.string.drawer_delete_title')),
      message: this.rs($r('app.string.drawer_delete_msg')),
      // Colors come from the palette, never literals -- ArkTS in this app holds no
      // hardcoded hex, which is what let the whole recolor happen in color.json alone.
      buttons: [
        { text: this.rs($r('app.string.drawer_cancel')), color: $r('app.color.text_secondary') },
        { text: this.rs($r('app.string.drawer_delete_ok')), color: $r('app.color.danger_text') }
      ]
    });
    if (answer.index !== 1) {
      return;
    }
    await SessionStore.remove(this.ctx(), id);
    this.sessions = await SessionStore.list(this.ctx());
    // Deleting the conversation on screen leaves it displayed but unowned; clearing the
    // id means the next message starts a new row instead of resurrecting the deleted one.
    if (id === this.sessionId) {
      this.sessionId = '';
    }
  }

  private assembleAssistant(): ChatMessage {
    const parts: MessagePart[] = [];
    if (this.streamingThinking.trim().length > 0) {
      parts.push({ kind: 'thinking', content: this.streamingThinking });
    }
    if (this.streaming.length > 0) {
      parts.push({ kind: 'text', content: this.streaming });
    }
    return { role: 'assistant', content: this.streaming, parts: parts };
  }

  // Abort the in-flight turn (slow/stuck response). Finalize the UI immediately — don't wait
  // for the transport (request.destroy() may not reject its promise promptly) — and keep
  // whatever streamed so far. Bumping turnSeq retires that turn's callbacks permanently.
  private onStop(): void {
    if (!this.busy) {
      return;
    }
    this.turnSeq++;
    this.canceler.stopped = true;
    this.canceler.cancel(); // best-effort: stop the network request
    if (this.streaming.length > 0 || this.streamingThinking.trim().length > 0) {
      this.history = this.history.concat(this.assembleAssistant());
      this.persist();
    }
    this.streaming = '';
    this.streamingThinking = '';
    this.busy = false;
    this.scrollToEnd();
  }

  private async onSend(): Promise<void> {
    const text = this.draft.trim();
    if (text.length === 0 || this.busy) {
      return;
    }
    if (this.activeModel.length === 0) {
      promptAction.showToast({ message: this.rs($r('app.string.chat_need_model')), duration: 2500 });
      return;
    }
    const userMsg: ChatMessage = { role: 'user', content: text };
    this.history = this.history.concat(userMsg);
    this.persist();
    this.draft = '';
    this.streaming = '';
    this.streamingThinking = '';
    this.laneNotice = '';
    this.busy = true;
    this.canceler = { cancel: () => {}, stopped: false };
    // Sending is an explicit "show me": re-arm even if they had scrolled up to read.
    this.stickToBottom = true;
    this.scrollToEnd();

    // This turn's identity: the callbacks below are live only while turnSeq still matches.
    const turnId = ++this.turnSeq;

    // Snapshot the model this turn runs on — the engine uses these, so a selection made
    // mid-flight can't retarget the request or misattribute a failure.
    const turnProviderId = this.activeProviderId;
    const turnModel = this.activeModel;

    const isLocalTurn = turnProviderId === LOCAL_PROVIDER_ID;
    const turnLocalPath = isLocalTurn
      ? (this.localPaths.get(turnModel) ?? '')
      : '';

    const turn: TurnContext = {
      providerId: turnProviderId,
      model: turnModel,
      // An explicit lane either way: a local pick must not be routed by the
      // heuristics, and a cloud pick already was forced before local existed.
      userForcedLane: isLocalTurn ? Lane.LOCAL : (this.activeHasKey ? Lane.CLOUD : ''),
      offline: false,
      localModelPath: turnLocalPath,
      onDecision: (decision: RouteDecision) => {
        if (turnId !== this.turnSeq) {
          return;
        }
        this.noticeFor(decision);
      }
    };

    let gotContent = false;
    const cb: StreamCallbacks = {
      onDelta: (t: string) => {
        if (turnId !== this.turnSeq) {
          return; // stopped or superseded
        }
        gotContent = true;
        this.streaming += t;
        this.scrollToEnd();
      },
      onThinking: (t: string) => {
        if (turnId !== this.turnSeq) {
          return;
        }
        this.streamingThinking += t;
        this.scrollToEnd();
      },
      onDone: () => {
        if (turnId !== this.turnSeq) {
          return; // onStop already finalized this turn, or a newer one owns the UI
        }
        if (this.streaming.length > 0 || this.streamingThinking.trim().length > 0) {
          this.history = this.history.concat(this.assembleAssistant());
          this.persist();
        }
        this.streaming = '';
        this.streamingThinking = '';
        this.busy = false;
        this.scrollToEnd();
        if (gotContent) {
          // A working turn resets the failure streak for this model.
          ConfigStore.clearFailure(this.ctx(), turnProviderId, turnModel);
        } else {
          promptAction.showToast({ message: this.rs($r('app.string.chat_empty_response')), duration: 3500 });
        }
      },
      onError: (code: number, message: string) => {
        // Check ownership BEFORE touching any state — a retired turn's late error must not
        // clear the streaming text or the busy flag of the turn that replaced it.
        if (turnId !== this.turnSeq) {
          return;
        }
        this.streaming = '';
        this.streamingThinking = '';
        this.busy = false;
        // 404/410 = model not callable on this account; count it and auto-remove after 3.
        if (code === 404 || code === 410) {
          this.handleModelFailure(turnProviderId, turnModel);
        } else {
          this.showError(code, message);
        }
      }
    };

    try {
      // The lane notice is delivered through turn.onDecision, not from here — by the time
      // this await resolves the whole turn is already over.
      await ChatEngine.send(this.ctx(), this.history, turn, cb, this.canceler);
    } catch (err) {
      if (turnId !== this.turnSeq) {
        return;
      }
      this.busy = false;
      const e = err as Error;
      this.showError(0, e.message);
    }
  }

  // A model returned 404/410 (listed but not callable on this account, e.g. NVIDIA kimi).
  // Count cumulative failures; after 3 auto-remove it from the dropdown and pick another.
  private async handleModelFailure(providerId: string, model: string): Promise<void> {
    if (providerId.length === 0 || model.length === 0) {
      this.showError(404, '');
      return;
    }
    const count = await ConfigStore.bumpFailure(this.ctx(), providerId, model);
    if (count >= 3) {
      await ConfigStore.removeAddedModel(this.ctx(), providerId, model);
      await ConfigStore.clearFailure(this.ctx(), providerId, model);
      await this.refresh();
      if (this.addedModels.length > 0) {
        const first = this.addedModels[0];
        await this.selectModel(first.providerId, first.id);
      } else {
        this.activeModel = '';
      }
      promptAction.showToast({ message: this.rs($r('app.string.chat_model_removed_failures'), model), duration: 4000 });
    } else {
      promptAction.showToast({
        message: this.rs($r('app.string.chat_call_failed_count'), count),
        duration: 3500
      });
    }
  }

  private noticeFor(decision: RouteDecision): void {
    if (decision.lane === Lane.CLOUD) {
      const p = findProvider(decision.providerId);
      const name = p !== undefined ? p.label : decision.providerId;
      this.laneNotice = this.rs($r('app.string.chat_cloud_notice'), name);
    } else {
      this.laneNotice = '';
    }
  }

  private showError(code: number, message: string): void {
    let hint: string;
    if (code === 429) {
      hint = this.rs($r('app.string.chat_err_busy'));
    } else if (code === 401 || code === 403) {
      hint = this.rs($r('app.string.chat_err_key'));
    } else if (code === 404 || code === 410) {
      hint = this.rs($r('app.string.chat_err_model_unavailable'));
    } else if (code >= 500) {
      hint = this.rs($r('app.string.chat_err_server'), code);
    } else if (message.length > 0) {
      hint = message;
    } else {
      hint = this.rs($r('app.string.chat_err_request'), code);
    }
    promptAction.showToast({ message: hint, duration: 3500 });
  }

  /** A finished message's display fields: thinking part (if any) and answer text. */
  private static thinkingOf(msg: ChatMessage): string {
    if (msg.parts !== undefined) {
      for (const p of msg.parts) {
        if (p.kind === 'thinking') {
          return p.content;
        }
      }
    }
    return '';
  }

  @Builder
  private bubble(msg: ChatMessage) {
    Row() {
      if (msg.role === 'user') {
        Text(msg.content)
          .fontSize(16 * this.fontScale)
          .fontColor($r('app.color.on_brand'))
          .backgroundColor($r('app.color.brand'))
          .borderRadius(14)
          .padding({ left: 14, right: 14, top: 10, bottom: 10 })
          .constraintSize({ maxWidth: '90%' })
      } else {
        // Full width, and definite on purpose: the code blocks inside size
        // against their parent, and a horizontal Scroll only engages when that
        // parent has a real width to clip to — a content-sized bubble would let
        // long code lines push it past the screen instead of scrolling.
        Column() {
          RichMessage({
            text: msg.content,
            thinking: Index.thinkingOf(msg),
            streaming: false,
            fontScale: this.fontScale
          })
        }
        .backgroundColor($r('app.color.bg_bubble'))
        .borderRadius(14)
        .padding({ left: 14, right: 14, top: 10, bottom: 10 })
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%')
    .justifyContent(msg.role === 'user' ? FlexAlign.End : FlexAlign.Start)
    .padding({ left: 12, right: 12, top: 4, bottom: 4 })
  }

  /** The in-flight assistant bubble — same shell, live thinking + growing text. */
  @Builder
  private streamingBubble() {
    Row() {
      Column() {
        RichMessage({
          text: this.streaming,
          thinking: this.streamingThinking,
          streaming: true,
          fontScale: this.fontScale
        })
      }
      .backgroundColor($r('app.color.bg_bubble'))
      .borderRadius(14)
      .padding({ left: 14, right: 14, top: 10, bottom: 10 })
      .width('100%')
      .alignItems(HorizontalAlign.Start)
    }
    .width('100%')
    .justifyContent(FlexAlign.Start)
    .padding({ left: 12, right: 12, top: 4, bottom: 4 })
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {

    // Offscreen render host: one invisible webview running MathJax + ECharts
    // for the whole page (core/RenderHost.ets). First child of the Stack so
    // everything paints over it; 1x1 + transparent, no touches.
    Web({ src: $rawfile('render/render.html'), controller: RenderHost.controller })
      .javaScriptAccess(true)
      .width(1)
      .height(1)
      .opacity(0)
      .hitTestBehavior(HitTestMode.None)
      .onPageEnd(() => {
        RenderHost.onHostReady();
      })

    Column() {
      // Top bar: the hamburger and the wordmark beside it, and nothing else. The gear
      // that used to sit on the trailing edge is a group inside the drawer now (see
      // HistoryDrawer.ets), which is what leaves this bar with a single affordance --
      // the same shape the web client's bar has.
      Row() {
        SymbolGlyph($r('sys.symbol.line_3_horizontal'))
          .fontSize(ICON.xxl)
          .fontColor([$r('app.color.text_primary')])
          .margin({ right: 14 })
          .onClick(() => {
            this.openDrawer();
          })
        // Serif, as the web wordmark is: it is the brand here, not a screen title, and
        // the app ships no other brand mark in the bar to carry that on its own.
        Text($r('app.string.app_name'))
          .fontSize(FONT.headline)
          .fontFamily('serif')
          .fontWeight(600)
          .fontColor($r('app.color.text_primary'))
          .layoutWeight(1)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 14, bottom: 14 })
      .border({ width: { bottom: 0.5 }, color: $r('app.color.border') })

      if (this.incognito) {
        Row() {
          SymbolGlyph($r('sys.symbol.eye_slash'))
            .fontSize(ICON.sm)
            .fontColor([$r('app.color.notice_fg')])
          Text($r('app.string.incognito_note'))
            .fontSize(FONT.caption)
            .fontColor($r('app.color.notice_fg'))
            .margin({ left: 8 })
            .layoutWeight(1)
        }
        .width('100%')
        .backgroundColor($r('app.color.notice_bg'))
        .padding({ left: 20, right: 20, top: 8, bottom: 8 })
      }

      if (this.laneNotice.length > 0) {
        Text(this.laneNotice)
          .fontSize(FONT.caption)
          .fontColor($r('app.color.notice_fg'))
          .backgroundColor($r('app.color.notice_bg'))
          .width('100%')
          .padding({ left: 20, right: 20, top: 8, bottom: 8 })
      }

      // Messages
      List({ scroller: this.scroller }) {
        ForEach(this.history, (msg: ChatMessage, index: number) => {
          ListItem() {
            this.bubble(msg)
          }
        }, (msg: ChatMessage, index: number) => index.toString() + msg.role)

        // `busy` and not just "has text yet": the bubble has to exist from the
        // moment the turn is sent, because the wait BEFORE the first token is
        // exactly what its waiting dots are there to cover (extraction from an
        // upload runs a vision model; a thinking model deliberates first).
        if (this.busy || this.streaming.length > 0 || this.streamingThinking.trim().length > 0) {
          ListItem() {
            this.streamingBubble()
          }
        }
      }
      .layoutWeight(1)
      .width('100%')
      .listDirection(Axis.Vertical)
      .edgeEffect(EdgeEffect.Spring)
      // ScrollState.Idle is what a CONTROLLER-driven scroll reports, i.e. our own
      // scrollEdge() — acting on it would immediately undo the stick it just honored.
      // Only a finger (Scroll) or its inertia (Fling) may change the decision.
      .onDidScroll((offset: number, state: ScrollState) => {
        if (state !== ScrollState.Idle) {
          this.syncStick();
        }
      })
      // A fling can settle a frame after its last onDidScroll; re-check once it rests
      // so stopping exactly at the bottom reliably resumes following.
      .onScrollStop(() => {
        this.syncStick();
      })

      // Composer — rounded pill: input on top, control row (attach · model picker ·
      // send) below.
      Column() {
        Column({ space: 6 }) {
          TextInput({ text: this.draft, placeholder: $r('app.string.chat_input_hint') })
            .fontSize(16 * this.fontScale)
            .backgroundColor(Color.Transparent)
            .borderRadius(0)
            .padding({ left: 10, right: 10, top: 8, bottom: 8 })
            .width('100%')
            .enabled(!this.busy)
            .onChange((v: string) => {
              this.draft = v;
            })
            .onSubmit(() => {
              this.onSend();
            })

          // Control row, three zones like the web composer on a phone: attach on the
          // leading edge, send on the trailing edge, and the model picker centred in
          // the flexible middle. The two ends are both 40x40, which is what makes the
          // middle optically centred rather than merely "the leftover space".
          Row() {
            // Attachment slot. Inert for now -- picking a file is easy (LocalModelStore
            // already drives DocumentViewPicker), but neither transport can carry one:
            // ChatMessage is text-only and both lanes map messages to bare
            // {role, content} before sending, so a staged file would be silently
            // dropped at send. It holds its 40x40 here so the row's geometry is the
            // final one and wiring it later moves nothing on screen.
            Button({ type: ButtonType.Circle }) {
              SymbolGlyph($r('sys.symbol.paperclip'))
                .fontSize(ICON.md)
                .fontColor([$r('app.color.text_tertiary')])
            }
            .width(40)
            .height(40)
            .backgroundColor(Color.Transparent)
            .enabled(false)

            Row() {
              Row() {
                Text(this.pickerLabel())
                  .fontSize(14 * this.fontScale)
                  .fontColor($r('app.color.text_primary'))
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                Text(' ▾')
                  .fontSize(ICON.xs)
                  .fontColor($r('app.color.text_tertiary'))
              }
              // Clamp to the slot so a long model name ellipsizes instead of shoving
              // the send button off the trailing edge.
              .constraintSize({ maxWidth: '100%' })
              .padding({ left: 10, right: 10, top: 8, bottom: 8 })
              .onClick(() => {
                this.showPicker = true;
              })
            }
            .layoutWeight(1)
            .justifyContent(FlexAlign.Center)

            // While busy this becomes a STOP button (tap to interrupt a slow response).
            Button({ type: ButtonType.Circle }) {
              if (this.busy) {
                Stack({ alignContent: Alignment.Center }) {
                  LoadingProgress().width(30).height(30).color($r('app.color.on_brand'))
                  Text('■').fontSize(ICON.xs).fontColor($r('app.color.on_brand'))
                }
              } else {
                Text('↑').fontSize(ICON.lg).fontColor($r('app.color.on_brand'))
              }
            }
            .width(40)
            .height(40)
            .backgroundColor($r('app.color.brand'))
            .enabled(this.busy || this.draft.trim().length > 0)
            .onClick(() => {
              if (this.busy) {
                this.onStop();
              } else {
                this.onSend();
              }
            })
          }
          .width('100%')
          .alignItems(VerticalAlign.Center)
        }
        .width('100%')
        .padding({ left: 12, right: 12, top: 10, bottom: 10 })
        .borderRadius(24)
        .border({ width: 1, color: $r('app.color.border') })
        .backgroundColor($r('app.color.bg_surface'))
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 10, bottom: 16 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor($r('app.color.bg_page'))

    // Model picker overlay — an in-tree Stack layer (not bindPopup), so it re-renders
    // reactively when the added-model list changes (e.g. right after a delete).
    if (this.showPicker) {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor($r('app.color.scrim'))
        .onClick(() => {
          this.showPicker = false;
        })
      Column() {
        this.pickerPanel()
      }
      .borderRadius(12)
      .clip(true)
      .backgroundBlurStyle(BlurStyle.COMPONENT_ULTRA_THICK)
      .shadow({ radius: 16, color: $r('app.color.shadow'), offsetX: 0, offsetY: 2 })
      .margin({ bottom: 84 })
    }

    // Left navigation drawer -- an in-tree layer for the same reason as the picker
    // above. Declared AFTER it so a drawer opened while the picker is up paints over it.
    if (this.showDrawer) {
      // No separate scrim layer: the drawer's own root carries the dim AND the
      // tap-to-dismiss. A sibling scrim under a full-width drawer root never receives
      // the tap, which is how blank-area dismissal was broken to begin with.
      HistoryDrawer({
        sessions: this.sessions,
        activeId: this.sessionId,
        incognito: this.incognito,
        busy: this.busy,
        onNewChat: () => { this.newChat(); },
        onIncognito: () => { this.toggleIncognito(); },
        onPick: (id: string) => { this.pickSession(id); },
        onDelete: (id: string) => { this.confirmDelete(id); },
        onClose: () => { this.showDrawer = false; },
        onLanguage: () => { this.langController.open(); },
        onFontSize: () => { this.fontController.open(); },
        onTheme: () => { this.themeController.open(); },
        healthLastSync: this.healthLastSync,
        onHealthData: () => { this.healthController.open(); },
        onNativeProbe: () => { router.pushUrl({ url: 'pages/NativeProbe' }); }
      })
    }
    }
    .width('100%')
    .height('100%')
  }
}

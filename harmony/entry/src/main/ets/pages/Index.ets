import { common } from '@kit.AbilityKit';
import { promptAction, display, router } from '@kit.ArkUI';
import { ChatMessage, MessagePart, StreamCallbacks, Lane, RouteDecision, Canceler } from '../model/Chat';
import { RichMessage } from '../components/RichMessage';
import { RenderHost } from '../core/RenderHost';
import { ChatEngine, TurnContext, TurnResult } from '../core/ChatEngine';
import { findProvider, BUILTIN_PROVIDERS } from '../model/Providers';
import { SecureKeyStore } from '../core/SecureKeyStore';
import { ConfigStore, ActiveSelection, AddedModel } from '../core/ConfigStore';
import { ProviderManagerDialog } from '../components/ProviderManagerDialog';
import { AddModelDialog } from '../components/AddModelDialog';
import { LanguageDialog, FontSizeDialog, AboutDialog } from '../components/AppSettingsDialogs';

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
  // The models the user added via the "add model" dialog — the only ones shown in the picker.
  @State private addedModels: AddedModel[] = [];
  // Snapshot of added-model keys taken when the add dialog opens, so on close we can detect
  // what was newly added this session and auto-select the last one.
  private addedSnapshot: Set<string> = new Set<string>();
  @State private showPicker: boolean = false;
  // Picker list cap — computed from screen height so it only scrolls when it truly won't fit.
  @State private pickerMaxHeight: number = 320;
  private scroller: Scroller = new Scroller();
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

  private aboutController: CustomDialogController = new CustomDialogController({
    builder: AboutDialog(),
    autoCancel: true,
    alignment: DialogAlignment.Center,
    backgroundBlurStyle: BlurStyle.COMPONENT_ULTRA_THICK,
    cornerRadius: 16,
    width: '90%'
  });

  private settingsMenu(): MenuElement[] {
    return [
      { value: $r('app.string.settings_language'), action: () => { this.langController.open(); } },
      { value: $r('app.string.settings_font_size'), action: () => { this.fontController.open(); } },
      { value: $r('app.string.settings_about'), action: () => { this.aboutController.open(); } },
      // Temporary: step-0 native bridge probe. Remove with pages/NativeProbe.ets
      // once the real native chat lane is wired.
      { value: 'Native probe', action: () => { router.pushUrl({ url: 'pages/NativeProbe' }); } }
    ];
  }

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
    this.addedModels = await ConfigStore.getAddedModels(this.ctx());
    this.hasAnyKey = await this.anyKeyConfigured();
    const sel: ActiveSelection = await ConfigStore.load(this.ctx());
    this.activeProviderId = sel.providerId;
    // Keep the selection only if that model is still added AND its provider still has a key
    // (a model may have been removed via the picker / add-model dialog, or its cloud service
    // deleted — and legacy data may hold an added model whose key is already gone).
    const hasKey = await SecureKeyStore.hasKey(sel.providerId);
    if (hasKey && this.isAddedModel(sel.providerId, sel.model)) {
      this.activeModel = sel.model;
      this.activeHasKey = true;
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
    this.activeHasKey = await SecureKeyStore.hasKey(providerId);
    await ConfigStore.save(this.ctx(), { providerId: providerId, model: model });
  }

  // Remove a model straight from the picker (keeps the popup open to remove more). If it was
  // the active one, clear the selection back to "选择模型".
  private async removeModel(m: AddedModel): Promise<void> {
    const wasActive = this.activeProviderId === m.providerId && this.activeModel === m.id;
    await ConfigStore.removeAddedModel(this.ctx(), m.providerId, m.id);
    await this.refresh();
    if (wasActive) {
      this.activeModel = '';
    }
    promptAction.showToast({ message: this.rs($r('app.string.toast_removed'), m.displayName), duration: 1500 });
  }

  private pickerLabel(): string {
    if (this.activeModel.length === 0 || !this.activeHasKey) {
      return this.rs($r('app.string.chat_pick_model'));
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
              .fontSize(12)
              .fontColor('#9A9A9A')
              .width('100%')
              .padding({ left: 16, right: 16, top: 10, bottom: 4 })
            ForEach(g.models, (m: AddedModel) => {
              Row() {
                Text(m.displayName)
                  .fontSize(15)
                  .layoutWeight(1)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .onClick(() => {
                    this.showPicker = false;
                    this.selectModel(m.providerId, m.id);
                  })
                Button({ type: ButtonType.Circle }) {
                  Text('－').fontSize(18).fontColor(Color.White).fontWeight(FontWeight.Bold)
                }
                .width(28)
                .height(28)
                .margin({ left: 12 })
                .backgroundColor('#D24C57')
                .onClick(() => {
                  this.removeModel(m);
                })
              }
              .width('100%')
              .padding({ left: 16, right: 24, top: 14, bottom: 14 })
              .alignItems(VerticalAlign.Center)

              Divider()
                .strokeWidth(0.5)
                .color('#F0F0F0')
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
        .color('#E0E0E0')
        .margin({ top: 6, bottom: 2 })
      if (this.hasAnyKey) {
        Row() {
          Text($r('app.string.chat_menu_config_model')).fontSize(15).fontColor('#3F7FC1')
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
        Text($r('app.string.chat_menu_config_cloud')).fontSize(15).fontColor('#3F7FC1')
      }
      .width('100%')
      .padding(16)
      .onClick(() => {
        this.showPicker = false;
        this.mgrController.open();
      })
    }
    .width('90%')
    .padding({ top: 8, bottom: 8, left: 6, right: 6 })
  }

  private scrollToEnd(): void {
    this.scroller.scrollEdge(Edge.Bottom);
  }

  // The finished turn as a history message. `content` carries ONLY the answer
  // text (it goes back to the model as context — replaying chain-of-thought
  // would bloat and skew later turns); the thinking survives in parts, for
  // display alone.
  private assembleAssistant(): ChatMessage {
    const parts: MessagePart[] = [];
    if (this.streamingThinking.length > 0) {
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
    if (this.streaming.length > 0 || this.streamingThinking.length > 0) {
      this.history = this.history.concat(this.assembleAssistant());
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
    this.draft = '';
    this.streaming = '';
    this.streamingThinking = '';
    this.laneNotice = '';
    this.busy = true;
    this.canceler = { cancel: () => {}, stopped: false };
    this.scrollToEnd();

    // This turn's identity: the callbacks below are live only while turnSeq still matches.
    const turnId = ++this.turnSeq;

    // Snapshot the model this turn runs on — the engine uses these, so a selection made
    // mid-flight can't retarget the request or misattribute a failure.
    const turnProviderId = this.activeProviderId;
    const turnModel = this.activeModel;

    const turn: TurnContext = {
      providerId: turnProviderId,
      model: turnModel,
      userForcedLane: this.activeHasKey ? Lane.CLOUD : '',
      offline: false
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
        if (this.streaming.length > 0 || this.streamingThinking.length > 0) {
          this.history = this.history.concat(this.assembleAssistant());
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
      const result: TurnResult = await ChatEngine.send(this.ctx(), this.history, turn, cb, this.canceler);
      if (turnId !== this.turnSeq) {
        return;
      }
      this.noticeFor(result.decision);
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
          .fontColor(Color.White)
          .backgroundColor('#3F7FC1')
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
        .backgroundColor('#F1F1F3')
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
      .backgroundColor('#F1F1F3')
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
      // Top bar
      Row() {
        Text($r('app.string.app_name'))
          .fontSize(20)
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        SymbolGlyph($r('sys.symbol.gearshape'))
          .fontSize(24)
          .fontColor(['#333333'])
          .bindMenu(this.settingsMenu())
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 14, bottom: 14 })
      .border({ width: { bottom: 0.5 }, color: '#E5E5E5' })

      if (this.laneNotice.length > 0) {
        Text(this.laneNotice)
          .fontSize(12)
          .fontColor('#9A6B1E')
          .backgroundColor('#FBEEDD')
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

        if (this.streaming.length > 0 || this.streamingThinking.length > 0) {
          ListItem() {
            this.streamingBubble()
          }
        }
      }
      .layoutWeight(1)
      .width('100%')
      .listDirection(Axis.Vertical)
      .edgeEffect(EdgeEffect.Spring)

      // Composer — rounded pill: input on top, control row (model picker · send) below.
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

          Row() {
            Row() {
              Text(this.pickerLabel())
                .fontSize(14 * this.fontScale)
                .fontColor('#333333')
                .maxLines(1)
                .textOverflow({ overflow: TextOverflow.Ellipsis })
              Text(' ▾')
                .fontSize(12)
                .fontColor('#8A8A8A')
            }
            .layoutWeight(1)
            .padding({ left: 10, right: 10, top: 8, bottom: 8 })
            .onClick(() => {
              this.showPicker = true;
            })

            // While busy this becomes a STOP button (tap to interrupt a slow response).
            Button({ type: ButtonType.Circle }) {
              if (this.busy) {
                Stack({ alignContent: Alignment.Center }) {
                  LoadingProgress().width(30).height(30).color('#FFFFFF')
                  Text('■').fontSize(12).fontColor(Color.White)
                }
              } else {
                Text('↑').fontSize(20).fontColor(Color.White)
              }
            }
            .width(40)
            .height(40)
            .backgroundColor('#3F7FC1')
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
        .border({ width: 1, color: '#E0E0E0' })
        .backgroundColor('#FAFAFA')
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 10, bottom: 16 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.White)

    // Model picker overlay — an in-tree Stack layer (not bindPopup), so it re-renders
    // reactively when the added-model list changes (e.g. right after a delete).
    if (this.showPicker) {
      Column()
        .width('100%')
        .height('100%')
        .backgroundColor('#33000000')
        .onClick(() => {
          this.showPicker = false;
        })
      Column() {
        this.pickerPanel()
      }
      .borderRadius(12)
      .clip(true)
      .backgroundBlurStyle(BlurStyle.COMPONENT_ULTRA_THICK)
      .shadow({ radius: 16, color: '#26000000', offsetX: 0, offsetY: 2 })
      .margin({ bottom: 84 })
    }
    }
    .width('100%')
    .height('100%')
  }
}

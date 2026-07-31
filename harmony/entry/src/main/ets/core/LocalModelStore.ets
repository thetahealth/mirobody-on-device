/**
 * Where the on-device GGUF models live.
 *
 * HARD CONSTRAINT that shapes all of this: llama.cpp loads a model by PATH and
 * mmaps it (src/llm/local.cpp), so an ArkTS-readable URI or fd is not enough. And
 * a multi-GB download must survive an app uninstall, so the file cannot live in
 * app-private storage. Both are satisfied at once because a file the user picks
 * through DocumentViewPicker IS openable and mmap-able by native code at the path
 * behind its URI — verified on device (Mate 70 Pro+): the granted
 * `file://docs/storage/Users/currentUser/...` URI resolves to
 * `/storage/Users/currentUser/...`, which native opens with mmap succeeding.
 *
 * So there is no copy step anywhere here. A model is a REFERENCE to a file the
 * user owns; removing it from the app never deletes the file.
 *
 * Two acquisition sources, mirroring the Android/iOS clients:
 *   import  — user picks an existing .gguf. This is also the survive-uninstall path.
 *   folder  — user picks a directory to download into. Needed because
 *             Environment.getUserDownloadDir() answers 801 "device doesn't support
 *             this api" on real HarmonyOS devices, so a public dir cannot simply be
 *             derived; the user names one instead (which is better anyway).
 *
 * Grants are persisted with @ohos.fileshare (the HarmonyOS analogue of an iOS
 * security-scoped bookmark) and re-activated on launch, which is what makes an
 * imported path still openable after a cold start. That needs
 * ohos.permission.FILE_ACCESS_PERSIST declared, or persistPermission answers 201.
 */
import { common } from '@kit.AbilityKit';
import { picker } from '@kit.CoreFileKit';
import { preferences } from '@kit.ArkData';
import { BusinessError } from '@kit.BasicServicesKit';
import fileUri from '@ohos.file.fileuri';
import fileShare from '@ohos.fileshare';
import fs from '@ohos.file.fs';
import hilog from '@ohos.hilog';

/**
 * Provider id the composer uses for on-device models. A sentinel rather than a real
 * ProviderRecord because there is no endpoint, no key and no catalog behind it —
 * but reusing the provider/model shape lets the picker, the selection and the
 * removal path stay lane-agnostic.
 */
export const LOCAL_PROVIDER_ID: string = '__local__';

const STORE_NAME = 'local_models';
const KEY_MODELS = 'models';
const KEY_DOWNLOAD_DIR = 'download_dir';

/** A model the app may load. `path` is what native mmaps; `uri` is what grants it. */
export interface LocalModel {
  id: string;           // file name — stable and unique enough for a picked file
  uri: string;          // picker-granted URI, the handle persistPermission needs
  path: string;         // absolute path resolved from the URI, for the engine
  displayName: string;
  sizeBytes: number;
  source: string;       // 'imported' | 'downloaded'
}

/** The directory the user chose to download into (empty when unset). */
export interface DownloadDir {
  uri: string;
  path: string;
}

function log(msg: string): void {
  hilog.info(0x3200, 'mirobody-models', '%{public}s', msg);
}

export class LocalModelStore {
  private static store: preferences.Preferences | null = null;
  // Memoized so activation happens exactly once per process, and so every entry
  // point can await it without callers coordinating.
  private static activation: Promise<void> | null = null;
  // Models dropped by the last activation sweep, waiting to be reported once.
  private static dropped: string[] = [];

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (LocalModelStore.store === null) {
      LocalModelStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return LocalModelStore.store;
  }

  //--------------------------------------------------------------------------
  // Models
  //--------------------------------------------------------------------------

  static async list(ctx: common.UIAbilityContext): Promise<LocalModel[]> {
    await LocalModelStore.ensureActivated(ctx);
    const p = await LocalModelStore.prefs(ctx);
    const raw = await p.get(KEY_MODELS, '[]') as string;
    try {
      return JSON.parse(raw) as LocalModel[];
    } catch (err) {
      return [];
    }
  }

  private static async save(ctx: common.UIAbilityContext, list: LocalModel[]): Promise<void> {
    const p = await LocalModelStore.prefs(ctx);
    await p.put(KEY_MODELS, JSON.stringify(list));
    await p.flush();
  }

  /**
   * Let the user pick a .gguf and remember it. Returns the model, or undefined if
   * they cancelled or the file turned out not to be loadable.
   *
   * The path is verified with fs.statSync before it is stored: a URI the picker
   * hands back is not a promise that the path behind it resolves, and finding out
   * at model-load time would surface as a confusing "could not load model".
   */
  static async importModel(ctx: common.UIAbilityContext): Promise<LocalModel | undefined> {
    const opts = new picker.DocumentSelectOptions();
    opts.maxSelectNumber = 1;
    opts.fileSuffixFilters = ['.gguf'];

    let uris: string[] = [];
    try {
      uris = await new picker.DocumentViewPicker(ctx).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      log('picker failed code=' + e.code + ' ' + e.message);
      return undefined;
    }
    if (uris.length === 0) {
      return undefined;   // cancelled
    }
    const uri = uris[0];
    await LocalModelStore.persist(uri);

    const path = new fileUri.FileUri(uri).path;
    let size = 0;
    try {
      size = fs.statSync(path).size;
    } catch (err) {
      const e = err as BusinessError;
      log('picked file not reachable at ' + path + ' code=' + e.code);
      return undefined;
    }

    const name = new fileUri.FileUri(uri).name;
    const model: LocalModel = {
      id: name, uri: uri, path: path, displayName: name,
      sizeBytes: size, source: 'imported'
    };

    const list = await LocalModelStore.list(ctx);
    const next: LocalModel[] = [];
    for (const m of list) {
      if (m.id !== model.id) { next.push(m); }   // re-importing replaces
    }
    next.push(model);
    await LocalModelStore.save(ctx, next);
    log('imported ' + name + ' (' + size + ' bytes) at ' + path);
    return model;
  }

  /**
   * Forget a model. Never touches the file: it belongs to the user, and deleting
   * an imported multi-GB download because they tapped "remove" in a list would be
   * indefensible. The grant is revoked so we stop holding access we don't use.
   */
  static async remove(ctx: common.UIAbilityContext, id: string): Promise<void> {
    const list = await LocalModelStore.list(ctx);
    const next: LocalModel[] = [];
    for (const m of list) {
      if (m.id === id) {
        try {
          await fileShare.revokePermission([{
            uri: m.uri, operationMode: fileShare.OperationMode.READ_MODE
          }]);
        } catch (err) {
          // Already gone / never persisted: nothing to release.
        }
      } else {
        next.push(m);
      }
    }
    await LocalModelStore.save(ctx, next);
    // The selection is not cleared here: Index.refresh() re-validates it against
    // this list and drops it if it no longer resolves. One owner, one place.
  }

  //--------------------------------------------------------------------------
  // Download directory
  //--------------------------------------------------------------------------

  /**
   * Let the user name a directory for downloads. FOLDER select mode, because
   * Environment.getUserDownloadDir() is unsupported on real devices (801), so the
   * public location has to come from the user rather than from the platform.
   */
  static async pickDownloadDir(ctx: common.UIAbilityContext): Promise<DownloadDir | undefined> {
    const opts = new picker.DocumentSelectOptions();
    opts.selectMode = picker.DocumentSelectMode.FOLDER;
    opts.maxSelectNumber = 1;

    let uris: string[] = [];
    try {
      uris = await new picker.DocumentViewPicker(ctx).select(opts);
    } catch (err) {
      const e = err as BusinessError;
      log('folder picker failed code=' + e.code + ' ' + e.message);
      return undefined;
    }
    if (uris.length === 0) {
      return undefined;
    }
    const uri = uris[0];
    // WRITE too: this directory is a download target, not just a read source.
    await LocalModelStore.persist(uri, true);

    const dir: DownloadDir = { uri: uri, path: new fileUri.FileUri(uri).path };
    const p = await LocalModelStore.prefs(ctx);
    await p.put(KEY_DOWNLOAD_DIR, JSON.stringify(dir));
    await p.flush();
    log('download dir set to ' + dir.path);
    return dir;
  }

  static async downloadDir(ctx: common.UIAbilityContext): Promise<DownloadDir | undefined> {
    const p = await LocalModelStore.prefs(ctx);
    const raw = await p.get(KEY_DOWNLOAD_DIR, '') as string;
    if (raw.length === 0) {
      return undefined;
    }
    try {
      return JSON.parse(raw) as DownloadDir;
    } catch (err) {
      return undefined;
    }
  }

  /**
   * Names dropped by the activation sweep, cleared by reading. The UI reports these
   * so a moved file explains itself instead of the model just vanishing.
   */
  static takeDropped(): string[] {
    const out = LocalModelStore.dropped;
    LocalModelStore.dropped = [];
    return out;
  }

  /**
   * Resolve one model by id, or undefined when it is not (or no longer) offered.
   *
   * There is deliberately NO "active local model" here: the app already has exactly
   * one active-selection store (ConfigStore's provider_id + model, with
   * LOCAL_PROVIDER_ID as the provider), and the composer's picker is lane-agnostic
   * by design. A second notion of "active" living here drifted from that one
   * immediately — importing from the main UI wrote ConfigStore while the probe page
   * read this, and saw nothing.
   */
  static async byId(ctx: common.UIAbilityContext, id: string): Promise<LocalModel | undefined> {
    for (const m of await LocalModelStore.list(ctx)) {
      if (m.id === id) { return m; }
    }
    return undefined;
  }

  //--------------------------------------------------------------------------
  // Grants
  //--------------------------------------------------------------------------

  private static async persist(uri: string, write: boolean = false): Promise<void> {
    const mode = write
      ? fileShare.OperationMode.READ_MODE | fileShare.OperationMode.WRITE_MODE
      : fileShare.OperationMode.READ_MODE;
    try {
      await fileShare.persistPermission([{ uri: uri, operationMode: mode }]);
    } catch (err) {
      const e = err as BusinessError;
      // 201 means ohos.permission.FILE_ACCESS_PERSIST is missing from
      // module.json5. Not fatal for THIS session — the grant is live until the
      // process dies — so keep going and let the next launch surface it.
      log('persistPermission failed code=' + e.code + ' ' + e.message);
    }
  }

  /**
   * Arm the grants once per process, before anything reads the model list. Folded
   * in here rather than left to callers because it is LOAD-BEARING and easy to
   * forget: measured on device, skipping it makes an imported model fail to open
   * with the file still present. Every accessor awaits it, so there is no ordering
   * for a caller to get wrong.
   */
  private static ensureActivated(ctx: common.UIAbilityContext): Promise<void> {
    if (LocalModelStore.activation === null) {
      LocalModelStore.activation = LocalModelStore.activateAll(ctx);
    }
    return LocalModelStore.activation;
  }

  /**
   * Re-arm every stored grant. Must run before the first model load of a session:
   * a persisted grant is dormant until activated, and without this the path opens
   * fine in the session that picked it and fails on every launch after.
   *
   * Entries whose grant no longer resolves are dropped, so a file the user moved
   * or deleted stops being offered instead of failing at load time.
   */
  static async activateAll(ctx: common.UIAbilityContext): Promise<void> {
    // Deliberately NOT list(): that awaits ensureActivated(), which is awaiting
    // this call — reading the preference directly avoids the self-deadlock.
    const p0 = await LocalModelStore.prefs(ctx);
    let list: LocalModel[] = [];
    try {
      list = JSON.parse(await p0.get(KEY_MODELS, '[]') as string) as LocalModel[];
    } catch (err) {
      list = [];
    }
    const dir = await LocalModelStore.downloadDir(ctx);

    const policies: fileShare.PolicyInfo[] = [];
    for (const m of list) {
      policies.push({ uri: m.uri, operationMode: fileShare.OperationMode.READ_MODE });
    }
    if (dir !== undefined) {
      policies.push({
        uri: dir.uri,
        operationMode: fileShare.OperationMode.READ_MODE | fileShare.OperationMode.WRITE_MODE
      });
    }
    if (policies.length === 0) {
      return;
    }
    try {
      await fileShare.activatePermission(policies);
    } catch (err) {
      const e = err as BusinessError;
      // Partial failure is normal (a moved file); the statSync sweep below decides
      // what actually survived rather than trusting this call's verdict.
      log('activatePermission code=' + e.code + ' ' + e.message);
    }

    const kept: LocalModel[] = [];
    const gone: string[] = [];
    for (const m of list) {
      try {
        fs.statSync(m.path);
        kept.push(m);
      } catch (err) {
        // Moving the file changes its path, which invalidates the stored reference.
        // Dropping it is right — a dead path would only fail later, at load time —
        // but it MUST be reported: silently losing a model the user imported reads
        // as the app forgetting, with no hint that re-importing is the fix.
        gone.push(m.displayName);
        log('dropping unreachable model ' + m.id + ' at ' + m.path);
      }
    }
    if (kept.length !== list.length) {
      LocalModelStore.dropped = gone;
      await LocalModelStore.save(ctx, kept);
    }
  }
}

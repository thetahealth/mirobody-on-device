/**
 * Non-secret BYOK config — the active provider selection and its (possibly edited) model.
 * Keys are NOT here; they live in SecureKeyStore. Persisted via preferences.
 */
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';
import { defaultProviderId, findProvider } from '../model/Providers';
import { i18n } from '@kit.LocalizationKit';

const STORE_NAME = 'byok_config';
const KEY_PROVIDER = 'provider_id';
const KEY_MODEL = 'model';
const KEY_TIME_PREFIX = 'keytime.';
const KEY_ADDED = 'added_models';
const KEY_FAIL_PREFIX = 'fail.';

export interface ActiveSelection {
  providerId: string;
  model: string;
}

/** A model the user explicitly added to the composer dropdown. */
export interface AddedModel {
  providerId: string;
  id: string;         // API model id
  displayName: string;
}

function isCNLocale(): boolean {
  const region = i18n.System.getSystemRegion();
  return region === 'CN';
}

export class ConfigStore {
  private static store: preferences.Preferences | null = null;

  private static async prefs(ctx: common.UIAbilityContext): Promise<preferences.Preferences> {
    if (ConfigStore.store === null) {
      ConfigStore.store = await preferences.getPreferences(ctx, STORE_NAME);
    }
    return ConfigStore.store;
  }

  /** Load the active selection, falling back to the region default on first run. */
  static async load(ctx: common.UIAbilityContext): Promise<ActiveSelection> {
    const p = await ConfigStore.prefs(ctx);
    const fallbackId = defaultProviderId(isCNLocale());
    const providerId = await p.get(KEY_PROVIDER, fallbackId) as string;
    const provider = findProvider(providerId);
    const fallbackModel = provider !== undefined ? provider.model : '';
    const model = await p.get(KEY_MODEL, fallbackModel) as string;
    return { providerId, model };
  }

  static async save(ctx: common.UIAbilityContext, sel: ActiveSelection): Promise<void> {
    const p = await ConfigStore.prefs(ctx);
    await p.put(KEY_PROVIDER, sel.providerId);
    await p.put(KEY_MODEL, sel.model);
    await p.flush();
  }

  // When each provider's key was saved (epoch millis). Non-secret, so it lives here rather
  // than in the secure store. Keyed `keytime.<providerId>`.
  static async setKeyTime(ctx: common.UIAbilityContext, providerId: string, ts: number): Promise<void> {
    const p = await ConfigStore.prefs(ctx);
    await p.put(KEY_TIME_PREFIX + providerId, ts);
    await p.flush();
  }

  static async getKeyTime(ctx: common.UIAbilityContext, providerId: string): Promise<number> {
    const p = await ConfigStore.prefs(ctx);
    return await p.get(KEY_TIME_PREFIX + providerId, 0) as number;
  }

  static async clearKeyTime(ctx: common.UIAbilityContext, providerId: string): Promise<void> {
    const p = await ConfigStore.prefs(ctx);
    await p.delete(KEY_TIME_PREFIX + providerId);
    await p.flush();
  }

  // The models the user added to the composer dropdown (persisted as a JSON array).
  static async getAddedModels(ctx: common.UIAbilityContext): Promise<AddedModel[]> {
    const p = await ConfigStore.prefs(ctx);
    const raw = await p.get(KEY_ADDED, '[]') as string;
    try {
      const list = JSON.parse(raw) as AddedModel[];
      return list;
    } catch (err) {
      return [];
    }
  }

  static async setAddedModels(ctx: common.UIAbilityContext, list: AddedModel[]): Promise<void> {
    const p = await ConfigStore.prefs(ctx);
    await p.put(KEY_ADDED, JSON.stringify(list));
    await p.flush();
  }

  static async removeAddedModel(ctx: common.UIAbilityContext, providerId: string, id: string): Promise<void> {
    const list = await ConfigStore.getAddedModels(ctx);
    const next: AddedModel[] = [];
    for (const a of list) {
      if (!(a.providerId === providerId && a.id === id)) {
        next.push(a);
      }
    }
    await ConfigStore.setAddedModels(ctx, next);
    // Drop the failure streak too, so re-adding this model later starts from a clean slate
    // instead of inheriting an old count that could auto-remove it prematurely.
    await ConfigStore.clearFailure(ctx, providerId, id);
  }

  // Drop every added model belonging to a provider — called when its key/cloud service is
  // deleted, so the picker never lists a model with no key behind it.
  static async removeAddedModelsForProvider(ctx: common.UIAbilityContext, providerId: string): Promise<void> {
    const list = await ConfigStore.getAddedModels(ctx);
    const next: AddedModel[] = [];
    for (const a of list) {
      if (a.providerId !== providerId) {
        next.push(a);
      } else {
        await ConfigStore.clearFailure(ctx, a.providerId, a.id); // clear each removed model's streak
      }
    }
    await ConfigStore.setAddedModels(ctx, next);
  }

  // Cumulative "model unavailable" failure count, keyed `fail.<providerId>::<id>`. Used to
  // auto-remove a model that repeatedly 404s (listed but not callable on this account).
  static async bumpFailure(ctx: common.UIAbilityContext, providerId: string, id: string): Promise<number> {
    const p = await ConfigStore.prefs(ctx);
    const k = KEY_FAIL_PREFIX + providerId + '::' + id;
    const count = (await p.get(k, 0) as number) + 1;
    await p.put(k, count);
    await p.flush();
    return count;
  }

  static async clearFailure(ctx: common.UIAbilityContext, providerId: string, id: string): Promise<void> {
    const p = await ConfigStore.prefs(ctx);
    await p.delete(KEY_FAIL_PREFIX + providerId + '::' + id);
    await p.flush();
  }
}

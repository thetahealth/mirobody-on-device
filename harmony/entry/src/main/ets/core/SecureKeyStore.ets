/**
 * Secure key store — HARD RULE (docs/privacy-tiers.md, "Guarantees we hold ourselves to"):
 * user keys live on-device only, in the platform secure store, addressed by provider_id.
 * Never sent to any Mirobody server, logged, or put in telemetry.
 *
 * Backed by ASSET Kit (device-level encrypted asset store). One asset per provider,
 * ALIAS = "byok.key.<providerId>".
 */
import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import { BusinessError } from '@kit.BasicServicesKit';

const ALIAS_PREFIX = 'byok.key.';

function aliasFor(providerId: string): Uint8Array {
  return new util.TextEncoder().encodeInto(ALIAS_PREFIX + providerId);
}

function toBytes(text: string): Uint8Array {
  return new util.TextEncoder().encodeInto(text);
}

function fromBytes(bytes: Uint8Array): string {
  return util.TextDecoder.create('utf-8').decodeToString(bytes);
}

export class SecureKeyStore {
  /** Store (or replace) the API key for a provider. */
  static async saveKey(providerId: string, key: string): Promise<void> {
    // Remove any existing asset first so a re-paste overwrites cleanly (add() rejects
    // duplicates), then add the fresh secret.
    await SecureKeyStore.deleteKey(providerId);

    // NOTE: no IS_PERSISTENT tag — that would require ohos.permission.STORE_PERSISTENT_DATA
    // and make asset.add() throw without it. Non-persistent is the right default anyway:
    // the key is cleared on app uninstall.
    const attr: asset.AssetMap = new Map();
    attr.set(asset.Tag.SECRET, toBytes(key));
    attr.set(asset.Tag.ALIAS, aliasFor(providerId));
    attr.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_FIRST_UNLOCKED);
    await asset.add(attr);
  }

  /** Return the stored key for a provider, or '' if none. */
  static async getKey(providerId: string): Promise<string> {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.ALIAS, aliasFor(providerId));
    query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
    try {
      const res: Array<asset.AssetMap> = await asset.query(query);
      if (res.length === 0) {
        return '';
      }
      const secret = res[0].get(asset.Tag.SECRET) as Uint8Array;
      return fromBytes(secret);
    } catch (err) {
      const e = err as BusinessError;
      if (e.code === asset.ErrorCode.NOT_FOUND) {
        return '';
      }
      throw err as Error;
    }
  }

  /** True if a key is stored for this provider. */
  static async hasKey(providerId: string): Promise<boolean> {
    const key = await SecureKeyStore.getKey(providerId);
    return key.length > 0;
  }

  /** Remove the key for a provider; a no-op if none exists. */
  static async deleteKey(providerId: string): Promise<void> {
    const query: asset.AssetMap = new Map();
    query.set(asset.Tag.ALIAS, aliasFor(providerId));
    try {
      await asset.remove(query);
    } catch (err) {
      const e = err as BusinessError;
      if (e.code === asset.ErrorCode.NOT_FOUND) {
        return;
      }
      throw err as Error;
    }
  }
}

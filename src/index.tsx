import { callable, definePlugin, toaster } from "@decky/api";
import {
  ButtonItem,
  DropdownItem,
  Field,
  PanelSection,
  PanelSectionRow,
  Router,
  SliderField,
  ToggleField,
  staticClasses,
} from "@decky/ui";
import { useEffect, useRef, useState } from "react";
import { MdHdrOn } from "react-icons/md";
import { Key, LANGUAGES, Lang, mapSteamLanguage, translate } from "./i18n";

type Profile = {
  enabled: boolean;
  sdr_nits: number;
  target_nits: number;
  expand_gamut: boolean;
  grading: boolean;
  brightness: number;
  contrast: number;
  saturation: number;
  temperature: number;
  gamma: number;
};

type GameEntry = { name: string; custom: boolean; profile: Profile };

type Status = {
  panel_hdr: boolean;
  game: boolean;
  enabled?: boolean;
  profile?: "game" | "global";
  profile_name?: string;
  sdr?: number;
  peak?: number;
};

type LayerState = "active" | "no_formats" | "error" | "loaded" | "none";

type State = {
  global: Profile;
  manage_hdr_output: boolean;
  language: string;
  games: Record<string, GameEntry>;
  current_app: { appid: string; name: string } | null;
  status: Status;
  layer_state: LayerState;
};

type Diagnosis = { verdict: Key; connector: string; colorspace: string };

const getState = callable<[], State>("get_state");
const setGlobal = callable<[Profile], State>("set_global");
const setManageHdrOutput = callable<[boolean], State>("set_manage_hdr_output");
const setLanguage = callable<[string], State>("set_language");
const setGame = callable<[string, string, boolean, Profile], State>("set_game");
const removeGame = callable<[string], State>("remove_game");
const appStarted = callable<[string, string], State>("app_started");
const appStopped = callable<[string], State>("app_stopped");
const reapply = callable<[], State>("reapply");
const diagnose = callable<[], Diagnosis>("diagnose");

/** Idioma de la interfaz de Steam (para la opción "Automático"). */
function steamLanguage(): Lang {
  const w = window as any;
  const candidates = [
    w.LocalizationManager?.m_rgLocalesToUse?.[0],
    w.LocalizationManager?.m_strLanguage,
    navigator.language,
  ];
  for (const c of candidates) if (typeof c === "string" && c) return mapSteamLanguage(c);
  return "en";
}

function resolveLang(setting: string | undefined): Lang {
  if (setting && setting !== "auto" && LANGUAGES.some((l) => l.code === setting)) return setting as Lang;
  return steamLanguage();
}

function appName(appid: string): string {
  const w = window as any;
  try {
    const ov = w.appStore?.GetAppOverviewByAppID?.(Number(appid));
    if (ov?.display_name) return ov.display_name;
  } catch {
    /* ignorar */
  }
  const running = Router.MainRunningApp as any;
  if (running && String(running.appid) === appid && running.display_name) return running.display_name;
  return `App ${appid}`;
}

/** Valor del desplegable para el perfil global. */
const GLOBAL = "__global__";

// La selección se guarda fuera del componente: Decky puede volver a montar el panel
// (por ejemplo al cerrar un desplegable) y el estado local de React se perdería.
let persistedSelection: string = GLOBAL;
let lastAutoSelectedApp: string | undefined;

/** Juegos instalados de la biblioteca de Steam (incluye accesos directos no-Steam). */
function libraryGames(): { appid: string; name: string }[] {
  const w = window as any;
  const out = new Map<string, string>();
  const add = (apps: any) => {
    try {
      for (const a of apps ?? []) {
        const id = a?.appid != null ? String(a.appid) : "";
        const name = a?.display_name;
        if (id && id !== "0" && name && !out.has(id)) out.set(id, String(name));
      }
    } catch {
      /* ignorar */
    }
  };
  add(w.collectionStore?.localGamesCollection?.allApps);
  add(w.collectionStore?.deckDesktopApps?.allApps);
  return [...out].map(([appid, name]) => ({ appid, name }));
}

// ---- Opciones de lanzamiento de Steam -------------------------------------------
// Algunos juegos (Elden Ring, Nightreign y otros de From Software) activan su propio
// HDR en cuanto la pantalla está en HDR, y se suma al de AutoHDR (imagen lavada).
// DXVK_HDR=0 hace que DXVK/vkd3d le digan al juego que la pantalla es SDR.
const HIDE_HDR_TOKEN = "DXVK_HDR=0";
const HIDE_HDR_RE = /(^|\s)DXVK_HDR=0(?=\s|$)/;

function getLaunchOptions(appid: string): Promise<string | null> {
  return new Promise((resolve) => {
    const sc = (window as any).SteamClient;
    let done = false;
    let reg: any = null;
    const finish = (v: string | null) => {
      if (done) return;
      done = true;
      try {
        reg?.unregister?.();
      } catch {
        /* ignorar */
      }
      resolve(v);
    };
    try {
      reg = sc?.Apps?.RegisterForAppDetails?.(Number(appid), (d: any) => finish(d?.strLaunchOptions ?? ""));
    } catch {
      finish(null);
    }
    setTimeout(() => finish(null), 3000);
  });
}

function withHideHdr(opts: string, enable: boolean): string {
  const current = opts.trim();
  if (!enable) {
    const cleaned = current.replace(HIDE_HDR_RE, " ").replace(/\s+/g, " ").trim();
    return cleaned === "%command%" ? "" : cleaned;
  }
  if (HIDE_HDR_RE.test(current) && !/(^|\s)DXVK_HDR=(?!0(\s|$))\S*/.test(current)) return current;
  // Quita cualquier otro DXVK_HDR=... (si hubiera DXVK_HDR=1 detrás, ganaría ese)
  const base = current.replace(/(^|\s)DXVK_HDR=\S*/g, " ").replace(/\s+/g, " ").trim();
  return withHideHdrClean(base);
}

function withHideHdrClean(current: string): string {
  if (current === "") return `${HIDE_HDR_TOKEN} %command%`;
  if (current.includes("%command%")) return `${HIDE_HDR_TOKEN} ${current}`;
  // Sin %command%, Steam añade las opciones como argumentos del juego
  return `${HIDE_HDR_TOKEN} %command% ${current}`;
}

function setLaunchOptions(appid: string, opts: string): boolean {
  try {
    (window as any).SteamClient.Apps.SetAppLaunchOptions(Number(appid), opts);
    return true;
  } catch (e) {
    console.error("AutoHDR: SetAppLaunchOptions falló", e);
    return false;
  }
}

type T = (key: Key, params?: Record<string, string | number>) => string;

function statusText(t: T, st: Status): string {
  const panel = t(st.panel_hdr ? "panel_hdr" : "panel_sdr");
  if (!st.game) return `${panel} · ${t("no_game")}`;
  const profile = st.profile === "game" ? t("profile_game", { name: st.profile_name ?? "" }) : t("profile_global");
  return `${panel} · ${
    st.enabled ? t("active", { profile, sdr: st.sdr ?? 0, peak: st.peak ?? 0 }) : t("inactive", { profile })
  }`;
}

const LAYER_KEYS: Record<LayerState, Key> = {
  active: "layer_active",
  no_formats: "layer_no_formats",
  error: "layer_error",
  loaded: "layer_loaded",
  none: "layer_none",
};

function ProfileEditor({ p, onChange, t }: { p: Profile; onChange: (p: Profile) => void; t: T }) {
  const set = (patch: Partial<Profile>) => onChange({ ...p, ...patch });
  return (
    <>
      <PanelSectionRow>
        <ToggleField label={t("toggle")} description={t("toggle_desc")} checked={p.enabled}
          onChange={(v) => set({ enabled: v })} />
      </PanelSectionRow>
      {p.enabled && (
        <>
          <PanelSectionRow>
            <SliderField label={t("paper")} description={t("paper_desc")} value={p.sdr_nits}
              min={80} max={600} step={10} showValue resetValue={200}
              onChange={(v) => set({ sdr_nits: v })} />
          </PanelSectionRow>
          <PanelSectionRow>
            <SliderField label={t("peak")} description={t("peak_desc")} value={p.target_nits}
              min={300} max={4000} step={50} showValue resetValue={1000}
              onChange={(v) => set({ target_nits: v })} />
          </PanelSectionRow>
          <PanelSectionRow>
            <ToggleField label={t("vivid")} description={t("vivid_desc")} checked={p.expand_gamut}
              onChange={(v) => set({ expand_gamut: v })} />
          </PanelSectionRow>
          <PanelSectionRow>
            <ToggleField label={t("grading")} description={t("grading_desc")} checked={p.grading}
              onChange={(v) => set({ grading: v })} />
          </PanelSectionRow>
          {p.grading && (
            <>
              <PanelSectionRow>
                <SliderField label={t("brightness")} value={p.brightness} min={-100} max={100} step={1} showValue
                  resetValue={0} onChange={(v) => set({ brightness: v })} />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField label={t("contrast")} value={p.contrast} min={-100} max={100} step={1} showValue
                  resetValue={6} onChange={(v) => set({ contrast: v })} />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField label={t("saturation")} value={p.saturation} min={-100} max={100} step={1} showValue
                  resetValue={20} onChange={(v) => set({ saturation: v })} />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField label={t("temperature")} description={t("temperature_desc")}
                  value={p.temperature} min={-3000} max={3000} step={100} showValue
                  resetValue={0} onChange={(v) => set({ temperature: v })} />
              </PanelSectionRow>
              <PanelSectionRow>
                <SliderField label={t("gamma")} description={t("gamma_desc")}
                  value={p.gamma} min={-40} max={40} step={1} showValue
                  resetValue={0} onChange={(v) => set({ gamma: v })} />
              </PanelSectionRow>
            </>
          )}
        </>
      )}
    </>
  );
}

function Content() {
  const [state, setState] = useState<State | null>(null);
  const [loadFailed, setLoadFailed] = useState(false);
  const timer = useRef<number | undefined>(undefined);

  // Carga el estado; si el backend aún arranca o falla, reintenta unas veces
  // y después muestra un error en lugar de quedarse en "Cargando…".
  const load = async () => {
    setLoadFailed(false);
    for (let attempt = 0; attempt < 8; attempt++) {
      try {
        const s = await getState();
        if (s && s.global) {
          setState(s);
          return;
        }
      } catch (e) {
        console.error("AutoHDR: get_state falló", e);
      }
      await new Promise((r) => setTimeout(r, 1500));
    }
    setLoadFailed(true);
  };

  useEffect(() => {
    load();
    return () => window.clearTimeout(timer.current);
  }, []);

  // Perfil que se está editando: GLOBAL o el AppID de un juego (abierto o no).
  // Cuando se abre un juego, se selecciona automáticamente.
  const [selected, setSelectedState] = useState<string>(persistedSelection);
  const setSelected = (v: string) => {
    persistedSelection = v;
    setSelectedState(v);
  };
  const currentAppId = state?.current_app?.appid;
  useEffect(() => {
    // Solo al abrirse un juego nuevo; no pisa la elección del usuario en cada render
    if (currentAppId && currentAppId !== lastAutoSelectedApp) {
      lastAutoSelectedApp = currentAppId;
      setSelected(currentAppId);
    }
  }, [currentAppId]);

  // Lista de juegos instalados (se lee una vez al abrir el panel)
  const [library, setLibrary] = useState<{ appid: string; name: string }[]>([]);
  useEffect(() => {
    setLibrary(libraryGames());
  }, []);

  // ¿Tiene el juego seleccionado DXVK_HDR=0 en sus opciones de lanzamiento? (null = desconocido)
  const [hideNative, setHideNative] = useState<boolean | null>(null);
  useEffect(() => {
    setHideNative(null);
    if (selected === GLOBAL) return;
    let alive = true;
    getLaunchOptions(selected).then((o) => alive && setHideNative(o === null ? null : HIDE_HDR_RE.test(o)));
    return () => {
      alive = false;
    };
  }, [selected]);

  const lang = resolveLang(state?.language);
  const t: T = (key, params) => translate(lang, key, params);

  if (!state) {
    return (
      <PanelSection>
        <PanelSectionRow>
          <Field label={loadFailed ? t("load_error") : t("loading")} bottomSeparator="none" />
        </PanelSectionRow>
        {loadFailed && (
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={() => load()}>
              {t("retry")}
            </ButtonItem>
          </PanelSectionRow>
        )}
      </PanelSection>
    );
  }

  const running = state.current_app;

  // Juego seleccionado (null = perfil global)
  const nameOf = (id: string) =>
    state.games[id]?.name ?? library.find((g) => g.appid === id)?.name ?? (running?.appid === id ? running.name : appName(id));
  const app = selected === GLOBAL ? null : { appid: selected, name: nameOf(selected) };
  const game = app ? state.games[app.appid] : undefined;
  const editingGame = !!(app && game?.custom);
  const profile = editingGame ? game!.profile : state.global;

  // Opciones del desplegable: global, luego juegos con perfil (★), luego el resto, por nombre
  const gameIds = new Set<string>([...library.map((g) => g.appid), ...Object.keys(state.games)]);
  if (running) gameIds.add(running.appid);
  const sortedGames = [...gameIds]
    .map((id) => ({ id, name: nameOf(id), custom: !!state.games[id]?.custom }))
    .sort((a, b) => Number(b.custom) - Number(a.custom) || a.name.localeCompare(b.name));
  const targetOptions = [
    { data: GLOBAL, label: t("sec_global") },
    ...sortedGames.map((g) => ({
      data: g.id,
      label: `${g.custom ? "★ " : ""}${g.name}${running?.appid === g.id ? ` (${t("running_tag")})` : ""}`,
    })),
  ];

  const updateProfile = (p: Profile) => {
    setState((s) => {
      if (!s) return s;
      if (editingGame && app) {
        return { ...s, games: { ...s.games, [app.appid]: { ...s.games[app.appid], profile: p } } };
      }
      return { ...s, global: p };
    });
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(async () => {
      const ns = editingGame && app ? await setGame(app.appid, app.name, true, p) : await setGlobal(p);
      setState((s) => (s ? { ...s, status: ns.status, layer_state: ns.layer_state } : ns));
    }, 300);
  };

  const setGameMode = async (custom: boolean) => {
    if (!app) return;
    const base = game?.profile ?? { ...state.global };
    setState(await setGame(app.appid, app.name, custom, base));
  };

  const languageOptions = [
    { data: "auto", label: t("lang_auto") },
    ...LANGUAGES.map((l) => ({ data: l.code, label: l.name })),
  ];

  return (
    <>
      <PanelSection title={t("status")}>
        <PanelSectionRow>
          <Field label={statusText(t, state.status)} description={t(LAYER_KEYS[state.layer_state] ?? "layer_none")}
            bottomSeparator="none" />
        </PanelSectionRow>
      </PanelSection>

      <PanelSection title={t("edit_title")}>
        <PanelSectionRow>
          <DropdownItem
            label={t("select_game")}
            description={t("select_game_desc")}
            rgOptions={targetOptions}
            selectedOption={selected}
            onChange={(o) => setSelected(String(o.data))}
          />
        </PanelSectionRow>
      </PanelSection>

      {app && (
        <PanelSection title={t("game", { name: app.name })}>
          <PanelSectionRow>
            <DropdownItem
              label={t("game_config")}
              rgOptions={[
                { data: "global", label: t("use_global") },
                { data: "custom", label: t("use_custom") },
              ]}
              selectedOption={editingGame ? "custom" : "global"}
              onChange={(o) => setGameMode(o.data === "custom")}
            />
          </PanelSectionRow>
          {hideNative !== null && (
            <PanelSectionRow>
              <ToggleField
                label={t("hide_native")}
                description={t("hide_native_desc")}
                checked={hideNative}
                onChange={async (v) => {
                  const current = await getLaunchOptions(app.appid);
                  if (current === null) return;
                  if (setLaunchOptions(app.appid, withHideHdr(current, v))) {
                    setHideNative(v);
                    toaster.toast({ title: "AutoHDR", body: t("restart_game") });
                  }
                }}
              />
            </PanelSectionRow>
          )}
          {game && (
            <PanelSectionRow>
              <ButtonItem layout="below" onClick={async () => setState(await removeGame(app.appid))}>
                {t("forget")}
              </ButtonItem>
            </PanelSectionRow>
          )}
        </PanelSection>
      )}

      {app && !editingGame ? (
        // Juego sin perfil propio: no se muestran los sliders del global para no
        // cambiarlo por error; se ofrece crear el perfil del juego.
        <PanelSection title={t("sec_game")}>
          <PanelSectionRow>
            <Field description={t("uses_global_hint")} bottomSeparator="none" />
          </PanelSectionRow>
          <PanelSectionRow>
            <ButtonItem layout="below" onClick={() => setGameMode(true)}>
              {t("create_custom")}
            </ButtonItem>
          </PanelSectionRow>
        </PanelSection>
      ) : (
        <PanelSection title={editingGame ? t("sec_game") : t("sec_global")}>
          <ProfileEditor p={profile} onChange={updateProfile} t={t} />
        </PanelSection>
      )}

      <PanelSection title={t("options")}>
        <PanelSectionRow>
          <DropdownItem
            label={t("language")}
            rgOptions={languageOptions}
            selectedOption={state.language || "auto"}
            onChange={async (o) => setState(await setLanguage(String(o.data)))}
          />
        </PanelSectionRow>
        <PanelSectionRow>
          <ToggleField label={t("allow_hdr")} description={t("allow_hdr_desc")} checked={state.manage_hdr_output}
            onChange={async (v) => setState(await setManageHdrOutput(v))} />
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem layout="below" onClick={async () => setState(await reapply())}>
            {t("refresh")}
          </ButtonItem>
        </PanelSectionRow>
        <PanelSectionRow>
          <ButtonItem
            layout="below"
            onClick={async () => {
              const d = await diagnose();
              toaster.toast({
                title: t("diag_title"),
                body: `${t(d.verdict)} [${d.connector}, Colorspace=${d.colorspace}]`,
                duration: 10000,
              });
              setState(await getState());
            }}
          >
            {t("diagnose")}
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </>
  );
}

export default definePlugin(() => {
  const sc = (window as any).SteamClient;

  const reg = sc?.GameSessions?.RegisterForAppLifetimeNotifications?.((e: any) => {
    const appid = String(e.unAppID);
    if (e.bRunning) {
      setTimeout(() => appStarted(appid, appName(appid)), 1000);
    } else {
      appStopped(appid);
    }
  });

  const running = Router.MainRunningApp as any;
  if (running?.appid) {
    const id = String(running.appid);
    appStarted(id, running.display_name || appName(id));
  }

  return {
    name: "AutoHDR",
    titleView: <div className={staticClasses.Title}>AutoHDR</div>,
    content: <Content />,
    icon: <MdHdrOn />,
    onDismount() {
      reg?.unregister?.();
    },
  };
});

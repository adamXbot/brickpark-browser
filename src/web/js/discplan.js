// LEGOLAND portable -- what a disc, a folder or an archive holds, and where each
// piece goes in the game's file system.
//
// The page installs ONE layout whatever the source was, and it is the layout the
// shipped installer produced, because that is what the game's own paths name:
//
//   /gamedata/volumes/Legoland.res       RES_OpenVolume tries .\volumes\<stem>.res
//   /gamedata/volumes/Graphics1.res      (the retail install left the two graphics
//   /gamedata/volumes/Graphics2.res       volumes on the CD; here they sit together)
//   /gamedata/strings/stab.str           LoadStrings: .\strings\stab.str
//   /gamedata/IMusic/*.sty *.sgt *.bnd   musicthread.c: imusic\segtheme1.sgt ...
//   /gamedata/zbuffers/*.bnv             joust.c: Zbuffers\joustride.bnv
//   /gamedata/RollerCoaster/RollerCoaster/CreatedData/*
//                                        coaster7.c chdirs here
//   /gamedata/AD_*.avi Lego.TTF Legoland.icm EGC.BMP
//   /gamedata/speech/*.wav               PlayNarrationFile: speech\<name>
//   /gamedata/advisor/*.llv              host-side advisor frames (data pack only)
//
// That split was read off the fourth LEGOLAND image, whose INSTALL/ folder is a
// copy of an installed game, and it accounts for every one of main.z's 331
// members: 243 music files, 23 z-buffers, 33 coaster files, the string table,
// 29 root files, and legoland.exe + Uninst.dll, which the port does not need.
//
// Platform-neutral: entries are { path, size, file } and nothing here reads a
// byte, so the planner runs unchanged under node.

export const COASTER_DIR = 'RollerCoaster/RollerCoaster/CreatedData/';

export const VOLUMES = [
  { key: 'legoland.res', dest: 'volumes/Legoland.res' },
  { key: 'graphics1.res', dest: 'volumes/Graphics1.res' },
  { key: 'graphics2.res', dest: 'volumes/Graphics2.res' }
];

// Editions, recognised by the exact sizes of the three volumes. Only the first
// is the data this port is built and tested against; the others are known discs
// whose install files are laid out the same way but whose contents are untested.
export const EDITIONS = [
  { id: 'en', label: 'LEGOLAND (English)', verified: true,
    sizes: { 'legoland.res': 16424086, 'graphics1.res': 19962774, 'graphics2.res': 120357911 } },
  { id: 'es', label: 'LEGOLAND (Spanish)', verified: false,
    sizes: { 'legoland.res': 16583926, 'graphics1.res': 24855106, 'graphics2.res': 121245895 } },
  { id: 'cz', label: 'LEGOLAND (Czech re-release)', verified: false,
    sizes: { 'legoland.res': 16504510, 'graphics1.res': 20066264, 'graphics2.res': 120357911 } }
];

// Files an install carries that the port never opens.
const SKIP = new Set(['legoland.exe', 'uninst.dll', 'thumbs.db', 'desktop.ini', '.ds_store']);

function baseName(p) {
  const parts = p.split(/[\\/]/);
  return parts[parts.length - 1];
}

function parentDirs(p) {
  return p.split(/[\\/]/).slice(0, -1).map((s) => s.toLowerCase());
}

// Where one install file goes, by NAME alone (the source's own folders are not
// trusted: main.z has none and a copied install may have been flattened), or
// null when the port has no use for it.
export function installPath(name) {
  const base = baseName(name);
  const lower = base.toLowerCase();
  if (!lower || SKIP.has(lower)) return null;
  const dot = lower.lastIndexOf('.');
  const ext = dot >= 0 ? lower.slice(dot + 1) : '';
  if (lower === 'rollercoaster.txt' || lower === 'rollercoaster.obj') return COASTER_DIR + base;
  switch (ext) {
    case 'sty': case 'sgt': case 'bnd': return 'IMusic/' + base;
    case 'bnv': return 'zbuffers/' + base;
    case 'ltx': case 'lms': case 'lfm': case 'lpt': return COASTER_DIR + base;
    case 'str': return lower === 'stab.str' ? 'strings/' + base : null;
    case 'ttf': case 'icm': return base;
    // EGC.BMP is the install's one bitmap; the disc root's autorun.bmp,
    // Logo.bmp and setup.bmp belong to the Windows installer.
    case 'bmp': return lower === 'egc.bmp' ? base : null;
    // The advisor's clips are small and named; the FMV movies (Intro.avi,
    // win.avi, ...) are Indeo 5 with no decoder here and 200 MB between them.
    case 'avi': return lower.startsWith('ad_') ? base : null;
    default: return null;
  }
}

// The install files a complete edition has, by destination folder. Used to say
// "incomplete" rather than to refuse: the music is not played yet, and a coaster
// file short only matters in the levels that have coasters.
export const INSTALL_COUNTS = { IMusic: 243, zbuffers: 23, RollerCoaster: 33, strings: 1, root: 29 };

export function installGroup(dest) {
  const slash = dest.indexOf('/');
  return slash < 0 ? 'root' : dest.slice(0, slash);
}

const SAVE_RE = /^(profile[1-8]\.txt|[1-8]save[1-8]\.(sav|sh))$/i;

// Classify the entries of one source. Pure; the archive is only located here and
// expanded by the importer.
export function planSources(entries) {
  const plan = {
    volumes: {},            // key -> { dest, entry }
    archive: null,          // main.z entry
    install: [],            // loose install files: { dest, entry }
    speech: [],             // { dest, entry }
    saves: [],              // entries under a profiles/ folder
    cpack: false,           // the Czech re-release's packed install
    edition: null,
    problems: [],
    warnings: []
  };
  const byDepth = entries.slice().sort((a, b) =>
    a.path.split('/').length - b.path.split('/').length || a.path.localeCompare(b.path));
  const taken = new Set();
  for (const entry of byDepth) {
    const base = baseName(entry.path);
    const lower = base.toLowerCase();
    const dirs = parentDirs(entry.path);
    const vol = VOLUMES.find((v) => v.key === lower);
    if (vol) {
      if (!plan.volumes[vol.key]) plan.volumes[vol.key] = { dest: vol.dest, entry };
      continue;
    }
    if (lower === 'main.z') {
      if (!plan.archive) plan.archive = entry;
      continue;
    }
    if (lower === 'lego.pak' || lower === 'lland.pak') {
      plan.cpack = true;
      continue;
    }
    if (lower.endsWith('.wav') && dirs.includes('speech')) {
      const dest = 'speech/' + base;
      if (!taken.has(dest.toLowerCase())) {
        taken.add(dest.toLowerCase());
        plan.speech.push({ dest, entry });
      }
      continue;
    }
    if (SAVE_RE.test(base) && dirs.includes('profiles')) {
      plan.saves.push(entry);
      continue;
    }
    const dest = installPath(entry.path);
    if (dest && !taken.has(dest.toLowerCase())) {
      taken.add(dest.toLowerCase());
      plan.install.push({ dest, entry });
    }
  }

  const missing = VOLUMES.filter((v) => !plan.volumes[v.key]).map((v) => baseName(v.dest));
  if (missing.length)
    plan.problems.push('Missing ' + missing.join(', ') + '. These are on the LEGOLAND CD, next to the Speech folder.');

  const hasStrings = plan.install.some((i) => i.dest.toLowerCase() === 'strings/stab.str');
  if (!plan.archive && !hasStrings) {
    plan.problems.push(plan.cpack
      ? 'This is the Czech re-release, which packs the game\'s install files in a format this page cannot read. Use the original LEGO Media disc, or add the folder of an installed copy.'
      : 'The game\'s install files are missing: neither main.z (on the original CD) nor an installed LEGOLAND folder was found.');
  }

  if (!missing.length) {
    const sizes = {};
    for (const v of VOLUMES) sizes[v.key] = plan.volumes[v.key].entry.size;
    plan.edition = EDITIONS.find((e) => VOLUMES.every((v) => e.sizes[v.key] === sizes[v.key])) ||
                   { id: 'unknown', label: 'an unrecognised edition', verified: false };
    if (!plan.edition.verified)
      plan.warnings.push('This looks like ' + plan.edition.label + '. The browser version is built and tested against the English release; other editions may not work.');
  }
  if (!plan.speech.length)
    plan.warnings.push('No Speech folder was found, so the advisor and the tutorial will be silent.');
  return plan;
}

// After the archive has been listed: the complete install list, archive members
// first (so a loose copy of the same file does not replace the original), with
// a completeness check against INSTALL_COUNTS.
export function mergeInstall(plan, archiveMembers) {
  const out = [];
  const taken = new Set();
  for (const m of archiveMembers || []) {
    const dest = installPath(m.name);
    if (!dest || taken.has(dest.toLowerCase())) continue;
    taken.add(dest.toLowerCase());
    out.push({ dest, member: m });
  }
  for (const item of plan.install) {
    if (taken.has(item.dest.toLowerCase())) continue;
    taken.add(item.dest.toLowerCase());
    out.push(item);
  }
  const counts = {};
  for (const item of out) counts[installGroup(item.dest)] = (counts[installGroup(item.dest)] || 0) + 1;
  const short = Object.keys(INSTALL_COUNTS).filter((g) => (counts[g] || 0) < INSTALL_COUNTS[g]);
  const essentials = ['strings/stab.str', 'lego.ttf', 'legoland.icm'];
  const lacking = essentials.filter((e) => !taken.has(e));
  return { items: out, counts, short, lacking };
}

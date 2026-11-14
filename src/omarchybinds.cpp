#include "omarchybinds.h"

#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

namespace {

// The scanner evaluates the user's Hyprland Lua config against a stub `hl` that records
// every hl.bind() call. The stub follows the one in Omarchy's omarchy-menu-keybindings
// (MIT, David Heinemeier Hansson): a catch-all value stands in for every Hyprland API the
// config touches at load time, so loops and arithmetic over it cannot abort the scan.
// Output is one tab-separated line per described bind:
//   modmask, description, key, kind, arg
// with backslash, tab and newline escaped in the text fields.
const char *ScanScript = R"LUA(
local modifiers = { SHIFT = 1, CTRL = 4, CONTROL = 4, ALT = 8, SUPER = 64 }

local function split_keys(keys)
  local modmask, key = 0, ""
  for part in string.gmatch(tostring(keys or ""), "[^+]+") do
    local value = part:gsub("^%s+", ""):gsub("%s+$", "")
    local modifier = modifiers[string.upper(value)]
    if modifier then modmask = modmask + modifier else key = value end
  end
  return modmask, key
end

local function lua_literal(value)
  local t = type(value)
  if t == "string" then
    return string.format("%q", value)
  elseif t == "number" or t == "boolean" then
    return tostring(value)
  elseif t == "table" then
    local parts, keys = {}, {}
    local n = #value
    for i = 1, n do parts[#parts + 1] = lua_literal(value[i]) end
    for k in pairs(value) do
      if not (type(k) == "number" and k >= 1 and k <= n and math.floor(k) == k) then keys[#keys + 1] = k end
    end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
    for _, k in ipairs(keys) do
      local prefix
      if type(k) == "string" and k:match("^[%a_][%w_]*$") then prefix = k .. " = "
      else prefix = "[" .. lua_literal(k) .. "] = " end
      parts[#parts + 1] = prefix .. lua_literal(value[k])
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
  end
  return "nil"
end

local function call_expression(path, ...)
  local args = {}
  for i = 1, select("#", ...) do args[i] = lua_literal(select(i, ...)) end
  return path .. "(" .. table.concat(args, ", ") .. ")"
end

local function dispatcher(kind, arg, expr)
  return { __dialstudio_dispatcher = true, kind = kind or "", arg = arg or "", expr = expr or "" }
end

local function dsp_proxy(path)
  return setmetatable({ path = path }, {
    __index = function(self, key) return dsp_proxy(self.path .. "." .. tostring(key)) end,
    __call = function(self, ...)
      local first = ...
      local expr = call_expression(self.path, ...)
      if self.path == "hl.dsp.exec_cmd" and type(first) == "string" then
        return dispatcher("exec", first, expr)
      end
      return dispatcher("lua", expr, expr)
    end,
  })
end

local noop
noop = setmetatable({}, {
  __index = function(_, key)
    if type(key) == "number" and key ~= 1 and key ~= 0 then return nil end
    return noop
  end,
  __call = function() return noop end,
  __len = function() return 0 end,
  __lt = function() return false end,
  __le = function() return false end,
  __unm = function() return 0 end,
  __add = function() return 0 end,
  __sub = function() return 0 end,
  __mul = function() return 0 end,
  __div = function() return 0 end,
  __mod = function() return 0 end,
  __idiv = function() return 0 end,
  __pow = function() return 0 end,
  __concat = function(left, right)
    if rawequal(left, noop) then return tostring(right) end
    return tostring(left)
  end,
  __tostring = function() return "" end,
})

local raw_ipairs = ipairs
function ipairs(list)
  if rawequal(list, noop) then return function() end, list, 0 end
  return raw_ipairs(list)
end

local function escape(text)
  return (tostring(text):gsub("[\\\t\n\r]", { ["\\"] = "\\\\", ["\t"] = "\\t", ["\n"] = "\\n", ["\r"] = "\\r" }))
end

-- Binds in the order the config made them. hl.unbind() drops every earlier bind on the
-- chord, as in Hyprland, so a personal override replaces the Omarchy default instead of
-- being listed next to it.
local recorded = {}

hl = setmetatable({
  dsp = dsp_proxy("hl.dsp"),
  bind = function(keys, bind_dispatcher, opts)
    opts = opts or {}
    local modmask, key = split_keys(keys)
    local entry = { chord = modmask .. ":" .. string.lower(key) }
    if opts.description and opts.description ~= "" then
      local kind, arg = "", ""
      if type(bind_dispatcher) == "table" and bind_dispatcher.__dialstudio_dispatcher then
        kind = bind_dispatcher.kind or ""
        arg = bind_dispatcher.arg or bind_dispatcher.expr or ""
      elseif type(bind_dispatcher) == "string" and bind_dispatcher ~= "" then
        kind, arg = "exec", bind_dispatcher
      elseif o and o.bind_commands and o.bind_commands[bind_dispatcher] then
        kind, arg = "exec", o.bind_commands[bind_dispatcher]
      elseif o and o.bind_expressions and o.bind_expressions[bind_dispatcher] then
        kind, arg = "lua", o.bind_expressions[bind_dispatcher]
      end
      entry.line = table.concat({ tostring(modmask), escape(opts.description), escape(key), kind, escape(arg) }, "\t")
    end
    recorded[#recorded + 1] = entry
    return noop
  end,
  unbind = function(keys)
    local modmask, key = split_keys(keys)
    local chord = modmask .. ":" .. string.lower(key)
    for i = #recorded, 1, -1 do
      if recorded[i].chord == chord then table.remove(recorded, i) end
    end
    return noop
  end,
  get_config = function() return nil end,
}, { __index = function() return noop end })

local config = arg and arg[1]
local file = config and io.open(config, "r")
if not file then
  io.stderr:write("cannot read " .. tostring(config) .. "\n")
  os.exit(3)
end
file:close()

-- Stops a runaway loop in the config long before the process timeout does.
debug.sethook(function() error("instruction limit exceeded") end, "", 50000000)
local ok, err = pcall(dofile, config)
debug.sethook()
for _, entry in ipairs(recorded) do
  if entry.line then print(entry.line) end
end
if not ok then
  io.stderr:write("lua scan stopped: " .. tostring(err) .. "\n")
  -- Binds read before the failure are still valid and were printed already.
  os.exit(4)
end
)LUA";

QString unescape(const QString &text) {
    if (!text.contains('\\')) return text;
    QString out;
    for (int i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const QChar next = text[++i];
            out += next == 't' ? QChar('\t') : next == 'n' ? QChar('\n') : next == 'r' ? QChar('\r') : next;
        } else {
            out += text[i];
        }
    }
    return out;
}

QStringList modNames(int mask) {
    QStringList mods;
    if (mask & 64) mods << "SUPER";
    if (mask & 1) mods << "SHIFT";
    if (mask & 4) mods << "CTRL";
    if (mask & 8) mods << "ALT";
    return mods;
}

// What a bind runs, so two entries that run the same thing can count as one.
QString identity(const OmarchyBind &b) { return b.kind + "\x1f" + b.arg; }

} // namespace

QString OmarchyBind::combo() const { return (modNames(modmask) << key).join(" + "); }

QString OmarchyBind::comboKey() const { return canonicalCombo(combo()); }

// Modifiers in a fixed order, lower case, so "alt+shift+tab" equals "SHIFT + ALT + TAB".
QString OmarchyBind::canonicalCombo(const QString &text) {
    QStringList mods;
    QString key;
    for (const QString &raw : text.split('+', Qt::SkipEmptyParts)) {
        const QString part = raw.trimmed().toLower();
        if (part == "super" || part == "shift" || part == "ctrl" || part == "control" || part == "alt")
            mods << (part == "control" ? QStringLiteral("ctrl") : part);
        else key = part;
    }
    mods.sort();
    return (mods << key).join('+');
}

QString OmarchyBind::summary() const {
    if (kind == "exec") return arg;
    if (kind == "lua") return arg;
    return {};
}

QJsonObject OmarchyBind::toJson() const {
    QJsonObject o{{"description", description}, {"combo", combo()}, {"runnable", runnable}, {"kind", kind}};
    if (!arg.isEmpty()) o.insert("runs", arg);
    if (!runnable) o.insert("reason", reason);
    return o;
}

void OmarchyCatalog::classify(OmarchyBind *b) {
    b->runnable = false;
    if (b->kind == "exec") {
        if (b->arg.trimmed().isEmpty()) b->reason = "the bind runs an empty command";
        else b->runnable = true;
    } else if (b->kind == "lua") {
        // Only Hyprland dispatcher calls and Omarchy's workspace helpers are replayed; the
        // scan comes from the user's own config, but a bind's expression is still handed to
        // `hyprctl dispatch`, so anything else is refused.
        static const QRegularExpression allowed(R"(^(hl\.dsp\.[A-Za-z_][A-Za-z_0-9.]*|o\.workspaces\.[A-Za-z_][A-Za-z_0-9]*)\(.*\)$)",
                                                QRegularExpression::DotMatchesEverythingOption);
        if (allowed.match(b->arg).hasMatch()) b->runnable = true;
        else b->reason = "its Lua expression is not a Hyprland dispatcher call";
    } else {
        b->reason = "it is a Lua function; Hyprland reports it only as __lua and there is no command to run";
    }
    if (b->runnable) b->reason.clear();
}

QList<OmarchyBind> OmarchyCatalog::parse(const QByteArray &tsv) {
    QList<OmarchyBind> out;
    for (const QByteArray &raw : tsv.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.trimmed().isEmpty()) continue;
        const QStringList f = line.split('\t');
        if (f.size() < 5) continue;
        OmarchyBind b;
        bool number = false;
        b.modmask = f[0].toInt(&number);
        if (!number) continue;
        b.description = unescape(f[1]);
        b.key = unescape(f[2]);
        b.kind = f[3];
        b.arg = unescape(f.mid(4).join('\t'));
        if (b.description.isEmpty() || b.key.isEmpty()) continue;
        classify(&b);
        out << b;
    }
    return out;
}

int OmarchyCatalog::runnableCount() const {
    int n = 0;
    for (const auto &b : m_binds) n += b.runnable;
    return n;
}

OmarchyLookup OmarchyCatalog::find(const QString &description, const QString &combo) const {
    OmarchyLookup result;
    const QString wanted = description.trimmed();
    if (wanted.isEmpty()) { result.message = "no description given"; return result; }

    auto collect = [&](auto matches) {
        QList<OmarchyBind> hits;
        for (const auto &b : m_binds)
            if (matches(b.description)) hits << b;
        return hits;
    };
    QList<OmarchyBind> hits = collect([&](const QString &d) { return d == wanted; });
    if (hits.isEmpty()) hits = collect([&](const QString &d) { return d.compare(wanted, Qt::CaseInsensitive) == 0; });
    if (hits.isEmpty()) {
        // A unique substring is accepted so `--omarchy "volume up"` need not be exact, but
        // only when every hit has the same description (several chords for one action).
        const QList<OmarchyBind> partial = collect([&](const QString &d) { return d.contains(wanted, Qt::CaseInsensitive); });
        QSet<QString> names;
        for (const auto &b : partial) names.insert(b.description);
        if (names.size() > 1) {
            QStringList list = names.values();
            list.sort();
            result.status = OmarchyLookup::Status::Ambiguous;
            result.message = QStringLiteral("\"%1\" matches several shortcuts (%2); use the full description")
                                 .arg(wanted, list.mid(0, 6).join(", ") + (list.size() > 6 ? ", ..." : ""));
            return result;
        }
        hits = partial;
    }
    if (hits.isEmpty()) {
        result.message = QStringLiteral("no Omarchy shortcut described as \"%1\" (see: dialstudio shortcuts)").arg(wanted);
        return result;
    }

    // Prefer the entry on the remembered combo: the same description can label chords that
    // do different things ("Reveal active window on top").
    if (!combo.isEmpty()) {
        const QString key = OmarchyBind::canonicalCombo(combo);
        for (const auto &b : hits)
            if (b.comboKey() == key) { result.status = OmarchyLookup::Status::Found; result.bind = b; return result; }
    }
    QSet<QString> distinct;
    for (const auto &b : hits) distinct.insert(identity(b));
    if (distinct.size() > 1) {
        // Chords that do different things: runnable ones are preferred when only one is.
        QList<OmarchyBind> runnable;
        QSet<QString> runnableDistinct;
        for (const auto &b : hits)
            if (b.runnable) { runnable << b; runnableDistinct.insert(identity(b)); }
        if (runnableDistinct.size() == 1) { result.status = OmarchyLookup::Status::Found; result.bind = runnable.first(); return result; }
        QStringList combos;
        for (const auto &b : hits) combos << b.combo();
        result.status = OmarchyLookup::Status::Ambiguous;
        result.message = QStringLiteral("\"%1\" labels %2 shortcuts that do different things (%3); bind one with --combo")
                             .arg(hits.first().description).arg(distinct.size()).arg(combos.join("; "));
        return result;
    }
    // Same action under several chords: the first runnable one stands for all of them.
    result.status = OmarchyLookup::Status::Found;
    result.bind = hits.first();
    for (const auto &b : hits)
        if (b.runnable) { result.bind = b; break; }
    return result;
}

QJsonArray OmarchyCatalog::toJson() const {
    QJsonArray array;
    for (const auto &b : m_binds) array.append(b.toJson());
    return array;
}

// ---------------------------------------------------------------------------

QString OmarchyScan::defaultConfig() { return QDir::homePath() + "/.config/hypr/hyprland.lua"; }

QString OmarchyScan::defaultOmarchyPath() {
    const QString path = qEnvironmentVariable("OMARCHY_PATH");
    return path.isEmpty() ? QStringLiteral("/usr/share/omarchy") : path;
}

QByteArray OmarchyScan::script() { return QByteArray(ScanScript); }

OmarchyScan::OmarchyScan(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QList<OmarchyBind>>();
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(10000);
    connect(&m_timeout, &QTimer::timeout, this, [this] { m_process.kill(); });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_timeout.stop();
            emit finished(false, {}, "lua is not installed (Omarchy's shortcut scan needs it)");
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const bool timedOut = !m_timeout.isActive();
        m_timeout.stop();
        QList<OmarchyBind> binds;
        QString error;
        const bool ok = interpret(&m_process, &binds, &error);
        emit finished(ok && !timedOut, binds, timedOut ? QStringLiteral("the Lua scan timed out") : error);
    });
}

OmarchyScan::~OmarchyScan() {
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
}

void OmarchyScan::configure(QProcess *process, const QString &hyprConfig, const QString &omarchyPath) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("OMARCHY_PATH", omarchyPath);
    env.remove("LUA_INIT");
    env.remove("LUA_PATH");
    process->setProcessEnvironment(env);
    process->setProgram("lua");
    // -E ignores LUA_INIT/LUA_PATH; the script comes in on stdin and the config is arg[1].
    process->setArguments({"-E", "-", hyprConfig});
}

bool OmarchyScan::interpret(QProcess *process, QList<OmarchyBind> *out, QString *error) {
    *out = OmarchyCatalog::parse(process->readAllStandardOutput());
    const QString problem = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (process->exitStatus() != QProcess::NormalExit) {
        if (error) *error = "the Lua scan crashed";
        return false;
    }
    if (process->exitCode() == 0) return true;
    // A config that stops half way still yields the binds before the failure.
    if (error) *error = problem.isEmpty() ? QStringLiteral("the Lua scan failed") : problem.section('\n', 0, 0);
    return !out->isEmpty() && process->exitCode() == 4;
}

void OmarchyScan::start(const QString &hyprConfig, const QString &omarchyPath) {
    if (running()) return;
    configure(&m_process, hyprConfig, omarchyPath);
    m_process.start();
    m_process.write(script());
    m_process.closeWriteChannel();
    m_timeout.start();
}

bool OmarchyScan::run(const QString &hyprConfig, const QString &omarchyPath, QList<OmarchyBind> *out, QString *error,
                      int timeoutMs) {
    QProcess process;
    configure(&process, hyprConfig, omarchyPath);
    process.start();
    if (!process.waitForStarted(2000)) {
        if (error) *error = "lua is not installed (Omarchy's shortcut scan needs it)";
        return false;
    }
    process.write(script());
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        if (error) *error = "the Lua scan timed out";
        return false;
    }
    return interpret(&process, out, error);
}

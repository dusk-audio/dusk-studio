function permissionService(texts, buttonNames) {
    if (buttonNames.filter(function (name) { return name === "Allow"; }).length !== 1
        || !buttonNames.some(function (name) { return /^Don['\u2019]t Allow$/.test(name); }))
        return null;
    let service = null;
    for (const text of typeof texts === "string" ? [texts] : texts) {
        const request = /^(?:Allow\s+(.+?)\s+to|(.+?)\s+(?:would like|wants)\s+to)\s+access\b([\s\S]*)$/i.exec(text.trim());
        if (request === null) continue;
        if (!/^(?:"Dusk\s*Studio"|\u201cDusk\s*Studio\u201d|Dusk\s*Studio)$/.test(request[1] || request[2]))
            return null;
        const detail = request[3];
        if (/\b(camera|contacts|screen recording|full disk)\b/i.test(detail)) return null;
        const microphone = /\bmicrophone\b/i.test(detail);
        const removable = /\bremovable (volume|volumes|drive|drives)\b/i.test(detail);
        if (microphone === removable) return null;
        const next = microphone ? "microphone" : "removable-volumes";
        if (service !== null && service !== next) return null;
        service = next;
    }
    return service;
}

function attribute(element, name) {
    try { return String(element[name]()); }
    catch (_) { return ""; }
}

function contents(window) {
    return [window].concat(window.entireContents());
}

function validationApplication(pid, executable) {
    const app = $.NSRunningApplication.runningApplicationWithProcessIdentifier(pid);
    const url = app && app.executableURL;
    if (url === undefined || url === null) return null;
    const actual = ObjC.unwrap(url.path);
    if (actual === undefined || actual === null) return null;
    if (actual !== executable || ObjC.unwrap(app.bundleIdentifier) !== "audio.dusk.studio")
        throw new Error("PID does not belong to the expected validation app");
    const matches = $.NSWorkspace.sharedWorkspace.runningApplications.js.filter(function (candidate) {
        return ObjC.unwrap(candidate.bundleIdentifier) === "audio.dusk.studio"
            || /^Dusk\s*Studio$/.test(String(ObjC.unwrap(candidate.localizedName)));
    });
    if (matches.length !== 1 || Number(matches[0].processIdentifier) !== pid)
        throw new Error("Another Dusk Studio instance makes prompt ownership ambiguous");
    return app;
}

function run(argv) {
    ObjC.import("AppKit");
    ObjC.import("ApplicationServices");
    if (!$.AXIsProcessTrusted())
        throw new Error("Accessibility access is required for validation permission handling");
    const events = Application("System Events");
    const centers = events.processes.whose({bundleIdentifier: "com.apple.UserNotificationCenter"});
    if (argv.length === 1 && argv[0] === "--check-access") {
        centers().forEach(function (center) { center.windows().forEach(contents); });
        return JSON.stringify({accessibility: true, notificationCenter: true});
    }
    if (argv.length < 2 || argv.length > 3)
        throw new Error("usage: allow-prompts.js PID_FILE APP_BINARY [SECONDS]");
    const pidFile = argv[0], executable = argv[1];
    if (!/\/DuskStudio\.app\/Contents\/MacOS\/DuskStudio$/.test(executable))
        throw new Error("Expected a Dusk Studio validation app binary");
    const seconds = argv.length === 3 ? Number(argv[2]) : 600;
    if (!Number.isFinite(seconds) || seconds < 1 || seconds > 3600)
        throw new Error("SECONDS must be between 1 and 3600");
    const deadline = Date.now() + seconds * 1000;
    let seen = false;
    poll: while (Date.now() < deadline) {
        const rawPid = $.NSString.stringWithContentsOfFileEncodingError(pidFile, $.NSUTF8StringEncoding, null);
        const pidText = ObjC.unwrap(rawPid);
        if (pidText === undefined || pidText === null) {
            if (seen) return;
            delay(1);
            continue;
        }
        if (!/^\s*[1-9][0-9]*\s*$/.test(pidText))
            throw new Error("Invalid validation PID file");
        const pid = Number(pidText.trim());
        const app = validationApplication(pid, executable);
        if (app === null) {
            if (seen) return;
            delay(1);
            continue;
        }
        seen = true;
        for (const center of centers()) {
            for (const window of center.windows()) {
                const elements = contents(window);
                const texts = [], buttons = [];
                elements.forEach(function (element) {
                    const role = attribute(element, "role");
                    if (role === "AXStaticText" || role === "AXTextArea")
                        texts.push(attribute(element, "value"), attribute(element, "name"));
                    if (role === "AXButton")
                        buttons.push({name: attribute(element, "name"), element: element});
                });
                const service = permissionService(texts, buttons.map(function (button) { return button.name; }));
                if (service === null) continue;
                if (validationApplication(pid, executable) === null) return;
                buttons.filter(function (button) { return button.name === "Allow"; })[0].element.click();
                console.log(JSON.stringify({action: "allow", service: service, pid: pid, executable: executable}));
                app.activateWithOptions($.NSApplicationActivateIgnoringOtherApps);
                delay(1);
                continue poll;
            }
        }
        delay(1);
    }
    throw new Error("Validation permission handler exceeded its bounded lifetime");
}

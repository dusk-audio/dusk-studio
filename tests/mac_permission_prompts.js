const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const context = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, "../scripts/regress/mac/allow-prompts.js"), "utf8"), context);
const buttons = ["Don't Allow", "Allow"];
const cases = [
    ['"Dusk Studio" would like to access the microphone.', buttons, "microphone"],
    ['Allow "DuskStudio" to access your microphone?', ["Don\u2019t Allow", "Allow"], "microphone"],
    ['"Dusk Studio" would like to access files on a removable volume.', buttons, "removable-volumes"],
    ['"Other App" would like to access the microphone.', buttons, null],
    ['"Dusk Studio" would like to access your camera.', buttons, null],
    ['"Dusk Studio" would like to access your contacts.', buttons, null],
    ['"Dusk Studio" would like to access your Downloads folder.', buttons, null],
    ['"Dusk Studio" would like to access the microphone and camera.', buttons, null],
    ['"Dusk Studio" would like to access the microphone.', ["Cancel", "Allow"], null],
    ['"Dusk Studio" would like to access the microphone.', ["Don't Allow", "Allow", "Allow"], null],
    ['Dusk Studio: microphone access is enabled.', buttons, null],
    ['"Dusk Studio" would like to access the microphone.', ["Don't Allow", "Allow All"], null],
    ['\u201cDusk Studio\u201d would like to access the microphone.', buttons, "microphone"],
    ['"Dusk Studio Helper" would like to access the microphone.', buttons, null],
    ['"Other Dusk Studio" would like to access the microphone.', buttons, null],
    ['"Other App" would like to access the microphone. Used with Dusk Studio.', buttons, null],
    [['"Other App" would like to access the microphone.', 'Used with Dusk Studio'], buttons, null],
    [['Dusk Studio', '"Dusk Studio" would like to access the microphone.', 'Recording audio'], buttons, "microphone"]
];
cases.forEach(function ([text, names, expected]) {
    assert.equal(context.permissionService(text, names), expected, text);
});

function ownedRun(actualPath, bundle, extraApps = []) {
    let clicks = 0, now = 0, inspections = 0;
    const expected = "/validation/DuskStudio.app/Contents/MacOS/DuskStudio";
    const app = {executableURL: {path: actualPath}, bundleIdentifier: bundle,
        localizedName: "Dusk Studio", processIdentifier: 123,
        activateWithOptions() {}};
    const allow = {role: () => "AXButton", name: () => "Allow", click() {
        clicks += 1;
        app.executableURL.path = null;
    }};
    const window = {entireContents() {
        inspections += 1;
        return [allow, {role: () => "AXButton", name: () => "Don't Allow"},
            {role: () => "AXStaticText", value: () => cases[0][0]}];
    }};
    const sandbox = vm.createContext({console: {log() {}},
        ObjC: {import() {}, unwrap: value => value},
        $: {AXIsProcessTrusted: () => true,
            NSString: {stringWithContentsOfFileEncodingError: () => "123"},
            NSRunningApplication: {runningApplicationWithProcessIdentifier: () => app},
            NSWorkspace: {sharedWorkspace: {runningApplications: {js: [app, ...extraApps]}}}},
        Application: () => ({processes: {whose: () => () => [{windows: () => [window]}]}}),
        Date: {now: () => now}, delay: seconds => { now += seconds * 1000; }});
    vm.runInContext(fs.readFileSync(path.join(__dirname, "../scripts/regress/mac/allow-prompts.js"), "utf8"), sandbox);
    let error;
    try { sandbox.run(["/private/validation/pid.txt", expected, "5"]); }
    catch (caught) { error = caught; }
    return {clicks, inspections, error};
}
const owned = ownedRun("/validation/DuskStudio.app/Contents/MacOS/DuskStudio", "audio.dusk.studio");
assert.equal(owned.error, undefined);
assert.equal(owned.clicks, 1);
for (const [actualPath, bundle] of [
    ["/other/DuskStudio.app/Contents/MacOS/DuskStudio", "audio.dusk.studio"],
    ["/validation/DuskStudio.app/Contents/MacOS/DuskStudio", "other.application"]
]) {
    const rejected = ownedRun(actualPath, bundle);
    assert.match(String(rejected.error), /expected validation app/);
    assert.equal(rejected.clicks, 0);
    assert.equal(rejected.inspections, 0);
}
for (const other of [
    {bundleIdentifier: "audio.dusk.studio", localizedName: "Other", processIdentifier: 456},
    {bundleIdentifier: "other.application", localizedName: "Dusk Studio", processIdentifier: 456}
]) {
    const rejected = ownedRun("/validation/DuskStudio.app/Contents/MacOS/DuskStudio", "audio.dusk.studio", [other]);
    assert.match(String(rejected.error), /ownership ambiguous/);
    assert.equal(rejected.clicks, 0);
    assert.equal(rejected.inspections, 0);
}
context.ObjC = {unwrap: value => value};
for (const missing of [undefined, {}]) {
    context.$ = {NSRunningApplication: {runningApplicationWithProcessIdentifier: () => missing}};
    assert.equal(context.validationApplication(123, "/validation/DuskStudio.app/Contents/MacOS/DuskStudio"), null);
}
console.log(cases.length + " permission scope, 5 process ownership and 2 process exit checks passed");

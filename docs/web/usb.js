// SPDX-License-Identifier: GPL-2.0-or-later
//
// The WebUSB transport for mtp.js: the unit's MTP interface and its two bulk endpoints, picked in
// the browser's own prompt. What tools/mtpcp.py's UsbTransport does with libusb, and what each
// way of failing to open the unit is called on the page.
//
// EXPORTED API
//
//   USB_FILTERS                   the prompt's filter: the vendor only, so that every product the
//                                 unit shows itself as is listed
//   MTP_CLASSES                   (class, subclass, protocol) of an MTP interface: standard, vendor
//   usbSupported() -> bool
//   findMtpInterface(configuration) -> { number, alternate, epIn, epOut, mps } | null
//       configuration: a USBConfiguration, or anything shaped like one
//   openFailure(step, error, device = null) -> failure    what a failed step means
//   openMtp() -> Promise<{ ok: true, transport, device } | failure>
//       device = { name, vendorId, productId, number, classes }
//   failure = { ok: false, outcome: 'unsupported' | 'none' | 'no-mtp' | 'held' | 'blocked' | 'lost',
//       text, advice: string[], step: 'request' | 'open' | 'configure' | 'claim' | null,
//       error: { name, message } | null, device }
//   class UsbTransport(device, found)   mtp.js's transport: mps, write, read, close; onLost is
//       called once when the device goes away

export const USB_FILTERS = [{ vendorId: 0x2367 }];          // S-10a.001
export const MTP_CLASSES = [[0x06, 0x01, 0x01], [0xFF, 0x01, 0x01]];

export const OPEN_WORDS = {
    unsupported: ['This browser cannot open USB devices: it has no WebUSB.',
        'Use Chrome or Edge on a computer, or choose Download it.'],
    none: ['No device was chosen.',
        'Press the button again and pick the OP-XY. If it is not listed, check that MTP mode is open ' +
        'on the unit and that the cable carries data, or choose Download it.'],
    'no-mtp': ['The device chosen offers no MTP interface.',
        "Open MTP mode in the unit's developer menu, then try again."],
    held: ["The unit's MTP interface could not be claimed: another program, or the system's own driver, " +
        'holds it.',
    "On Windows the system's own MTP driver usually holds it, and the browser cannot take it over: " +
        'choose Download it.',
    'On a Mac, quit any program that opens cameras, phones or MTP devices (Image Capture, Photos, ' +
        'Android File Transfer, OpenMTP), then try again.'],
    blocked: ['The browser does not let this page use the unit\'s MTP interface.', 'Choose Download it.'],
    lost: ['The unit went away.', 'Check the cable and try again.'],
};

export function usbSupported() {
    return typeof navigator !== 'undefined' && 'usb' in navigator;
}

const triple = (a) => [a.interfaceClass, a.interfaceSubclass, a.interfaceProtocol];
const isMtp = (a) => MTP_CLASSES.some((c) => c.every((v, k) => v === triple(a)[k]));

export function findMtpInterface(configuration) {
    for (const ifc of (configuration && configuration.interfaces) || [])
        for (const alt of ifc.alternates || []) {
            if (!isMtp(alt))
                continue;
            let epIn = null, epOut = null, mps = 0;
            for (const ep of alt.endpoints || []) {
                if (ep.type !== 'bulk')
                    continue;
                mps = ep.packetSize & 0x7FF;
                if (ep.direction === 'in')
                    epIn = ep.endpointNumber;
                else
                    epOut = ep.endpointNumber;
            }
            if (epIn !== null && epOut !== null)
                return { number: ifc.interfaceNumber, alternate: alt.alternateSetting, epIn, epOut, mps };
        }
    return null;
}

const gone = (e) => !!e && (e.name === 'NotFoundError' || /disconnect/i.test(e.message || ''));

export function openFailure(step, e, device = null) {
    let outcome;
    if (step === null)
        outcome = e;                                // an outcome given by name
    else if (gone(e) && step !== 'request')
        outcome = 'lost';
    else if (step === 'request')
        outcome = e && e.name === 'NotFoundError' ? 'none' : 'unsupported';
    else if (e && e.name === 'SecurityError' && step === 'claim')
        outcome = 'blocked';
    else
        outcome = 'held';
    const [text, ...advice] = OPEN_WORDS[outcome];
    return { ok: false, outcome, text, advice, step,
        error: e && typeof e === 'object' ? { name: e.name || 'Error', message: e.message || String(e) } : null,
        device };
}

function describeDevice(d) {
    return { name: [d.manufacturerName, d.productName].filter(Boolean).join(' ') || 'a USB device',
        vendorId: d.vendorId, productId: d.productId, number: null, classes: [] };
}

export async function openMtp() {
    if (!usbSupported())
        return openFailure(null, 'unsupported');
    let device;
    try {
        device = await navigator.usb.requestDevice({ filters: USB_FILTERS });
    } catch (e) {
        return openFailure('request', e);
    }
    const info = describeDevice(device);
    const shut = () => device.close().catch(() => {});
    try {
        await device.open();
    } catch (e) {
        return openFailure('open', e, info);
    }
    try {
        if (!device.configuration)
            await device.selectConfiguration(1);
    } catch (e) {
        await shut();
        return openFailure('configure', e, info);
    }
    info.classes = ((device.configuration && device.configuration.interfaces) || []).flatMap((i) =>
        (i.alternates || []).map((a) => triple(a).map((v) => v.toString(16).padStart(2, '0')).join('/')));
    const found = findMtpInterface(device.configuration);
    if (!found) {
        await shut();
        return openFailure(null, 'no-mtp', info);
    }
    info.number = found.number;
    try {
        await device.claimInterface(found.number);
        if (found.alternate !== 0)
            await device.selectAlternateInterface(found.number, found.alternate);
    } catch (e) {
        await shut();
        return openFailure('claim', e, info);
    }
    return { ok: true, transport: new UsbTransport(device, found), device: info };
}

const usbError = (what, e, lost) => Object.assign(
    new Error(`${what}: ${e && e.name ? e.name + ': ' : ''}${e && e.message ? e.message : String(e)}`),
    { gone: lost || gone(e) });

export class UsbTransport {
    constructor(device, found) {
        this.device = device;
        this.f = found;
        this.mps = found.mps;
        this.pending = null;    // a read in flight, kept across a timeout
        this.lost = false;
        this.onLost = null;
        this.onDisconnect = (ev) => {
            if (ev.device !== device || this.lost)
                return;
            this.lost = true;
            if (this.onLost)
                this.onLost();
        };
        navigator.usb.addEventListener('disconnect', this.onDisconnect);
    }

    // WebUSB has no transfer timeout: a write that has not finished in time is given up on.
    async write(bytes, timeoutMs) {
        let timer;
        const late = new Promise((resolve) => {
            timer = setTimeout(() => resolve(null), timeoutMs);
        });
        let r;
        try {
            r = await Promise.race([this.device.transferOut(this.f.epOut, bytes), late]);
        } catch (e) {
            throw usbError('transferOut', e, this.lost);
        } finally {
            clearTimeout(timer);
        }
        if (r === null)
            throw Object.assign(new Error(`transferOut: no end after ${timeoutMs} ms`), { timeout: true });
        if (r.status !== 'ok') {
            if (r.status === 'stall')
                await this.device.clearHalt('out', this.f.epOut).catch(() => {});
            throw new Error(`transferOut: ${r.status}`);
        }
        if (r.bytesWritten !== bytes.length)
            throw new Error(`transferOut: ${r.bytesWritten} of ${bytes.length} bytes`);
    }

    // The next bulk IN transfer, or null when none ended within timeoutMs. A read that timed out
    // stays pending and is the first to be waited on again; mtp.js asks every read for the same
    // size, so the transfer in flight is the one it would have asked for.
    async read(size, timeoutMs) {
        if (!this.pending) {
            this.pending = this.device.transferIn(this.f.epIn, size);
            this.pending.catch(() => {});
        }
        let timer;
        const late = new Promise((resolve) => {
            timer = setTimeout(() => resolve(null), timeoutMs);
        });
        let r;
        try {
            r = await Promise.race([this.pending, late]);
        } catch (e) {
            this.pending = null;
            throw usbError('transferIn', e, this.lost);
        } finally {
            clearTimeout(timer);
        }
        if (r === null) {
            if (this.lost)
                throw usbError('transferIn', new Error('the device went away'), true);
            return null;
        }
        this.pending = null;
        if (r.status !== 'ok') {
            if (r.status === 'stall')
                await this.device.clearHalt('in', this.f.epIn).catch(() => {});
            throw new Error(`transferIn: ${r.status}`);
        }
        const d = r.data;
        return d ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : new Uint8Array(0);
    }

    async close() {
        navigator.usb.removeEventListener('disconnect', this.onDisconnect);
        try {
            await this.device.releaseInterface(this.f.number);
        } catch (e) {
            // already gone
        }
        try {
            await this.device.close();
        } catch (e) {
            // already gone
        }
    }
}

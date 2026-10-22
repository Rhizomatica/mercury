# Mercury on AltStore PAL

Mercury for iOS is distributed through AltStore PAL (EU, Brazil, Japan), not
the App Store. Apple notarizes each version; AltStore installs it from a
source we host.

## The listing (App Store Connect, app "Mercury iOS", Apple ID 6822504870)

Apple wants most of the App Store metadata even for notarization.

- **Subtitle:** Email and files over HF radio (Apple allows 30 characters)
- **Category:** Utilities (secondary: Productivity)
- **Age rating:** 4+, every question "None"/"No"
- **Support URL:** https://github.com/Rhizomatica/mercury
- **Marketing URL:** https://www.rhizomatica.org/hermes/
- **Privacy policy URL:** https://rhizomatica.github.io/mercury/PRIVACY.html
- **App privacy:** Data Not Collected
- **Copyright:** 2026 Rhizomatica
- **Keywords:** ham radio,HF,modem,amateur radio,Winlink,VARA,email,ARQ,SSB,emergency
- **Export compliance:** no non-exempt encryption (`ITSAppUsesNonExemptEncryption` false in Info.plist)
- **Screenshots:** iPhone 6.9" (1320x2868) and iPad 13" (2064x2752), from the simulator

**Description:**

> Mercury is a software modem for sending email, files and messages over HF
> radio, the kind of link you have when there is no internet, no phone
> network and no power grid worth speaking of.
>
> Connect your phone or tablet to a single-sideband transceiver through an
> audio interface, and Mercury turns the radio into a data link: it handles
> the waveform, the error correction, the retransmissions and the speed,
> and gives your application an ordinary TCP connection. It speaks the
> VARA TNC protocol, so Winlink clients such as Pat, and chat programs, can
> use it.
>
> - Adaptive speed, from a robust 32-tone MFSK mode that works below the
>   noise up to 16-QAM on a good channel.
> - Files arrive intact or you are told they did not.
> - Broadcast mode for one-to-many traffic.
> - The same modem as Mercury on Linux, Windows and macOS: they talk to each
>   other.
>
> Mercury is free software (GPL-3.0) from the HERMES project of Rhizomatica.
> It needs an amateur-radio licence to transmit.

**Review notes:** Mercury is a radio modem. Without a radio it listens to the
microphone and decodes nothing; Settings shows the audio devices, and the
TX test sends a modem signal through the speaker.

## Releasing a version

1. `make fyne-ui-ios-ipa` on the Mac, then upload with `xcrun altool
   --upload-app` (see the Makefile target for the signing setup).
2. In App Store Connect: the version's number is the build's
   `CFBundleShortVersionString` (1.9.19 for build 1.9.19.1; a new app's
   version starts as "1.0"), Review Type **Notarization**, attach the
   build, Add for Review, Submit to App Review.  The API does all of it
   but App Privacy, the EU trader (DSA) status and the review contact,
   which are web-only or personal data.
3. When Apple accepts it, it makes an alternative distribution package (ADP).
   AltStore is told (marketplace notifications on) and processes it;
   `GET https://api.altstore.io/adps/<ADP id>` gives its `downloadURL` when
   done.
4. Host the ADP unchanged (directory layout, `manifest.json` byte for byte)
   under `docs/altstore/adp/<build>/` on mercuryv2, then add the version at
   the top of `versions` in `docs/altstore/source.json` (`date`, `size`,
   `downloadURL` = the hosted `manifest.json`).  GitHub Pages serves
   mercuryv2:/docs, so the source is
   `https://rhizomatica.github.io/mercury/altstore/source.json`, with the
   icon and screenshots beside it.

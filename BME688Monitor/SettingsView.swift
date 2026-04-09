import SwiftUI

struct SettingsView: View {
    @Environment(APIService.self) var api
    @State private var settings = EditableSettings()
    @State private var isLoading = true
    @State private var isSaving = false
    @State private var saveMessage: String?
    @State private var showServerEdit = false

    var body: some View {
        @Bindable var bindableApi = api

        NavigationStack {
            Form {
                // Server connection
                Section {
                    HStack {
                        Image(systemName: "network")
                            .foregroundStyle(.blue)
                        VStack(alignment: .leading) {
                            Text("Server URL")
                                .font(.subheadline)
                            Text(api.serverURL)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                        Spacer()
                        HStack(spacing: 6) {
                            Circle()
                                .fill(api.isConnected ? .green : .red)
                                .frame(width: 8, height: 8)
                            Text(api.isConnected ? "Online" : "Offline")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .contentShape(Rectangle())
                    .onTapGesture { showServerEdit = true }
                } header: {
                    Text("Verbindung")
                }

                if !isLoading {
                    // General settings
                    Section {
                        HStack {
                            Label("UTC Offset", systemImage: "clock.arrow.circlepath")
                            Spacer()
                            TextField("min", value: $settings.utcOffsetMin, format: .number)
                                .keyboardType(.numbersAndPunctuation)
                                .multilineTextAlignment(.trailing)
                                .frame(width: 70)
                        }
                        HStack {
                            Label("Log-Tage", systemImage: "calendar")
                            Spacer()
                            Stepper("\(settings.logDays)", value: $settings.logDays, in: 1...30)
                                .frame(width: 140)
                        }
                    } header: {
                        Text("Allgemein")
                    }

                    // Sensor settings
                    Section {
                        HStack {
                            Label("Leseintervall", systemImage: "timer")
                            Spacer()
                            Text("\(settings.sensorIntervalMs) ms")
                                .foregroundStyle(.secondary)
                                .monospacedDigit()
                            Stepper("", value: $settings.sensorIntervalMs, in: 50...2000, step: 50)
                                .labelsHidden()
                                .frame(width: 100)
                        }
                        HStack {
                            Label("Gas Intervall", systemImage: "flame")
                            Spacer()
                            Text("\(settings.gasIntervalS) s")
                                .foregroundStyle(.secondary)
                                .monospacedDigit()
                            Stepper("", value: $settings.gasIntervalS, in: 5...300, step: 5)
                                .labelsHidden()
                                .frame(width: 100)
                        }
                        Toggle(isOn: $settings.heaterFilter) {
                            Label("Heater-Filter", systemImage: "thermometer.variable.and.figure")
                        }
                        if settings.heaterFilter {
                            HStack {
                                Label("Blanking", systemImage: "hourglass")
                                Spacer()
                                Text("\(settings.heaterBlankingMs) ms")
                                    .foregroundStyle(.secondary)
                                    .monospacedDigit()
                                Stepper("", value: $settings.heaterBlankingMs, in: 250...6000, step: 250)
                                    .labelsHidden()
                                    .frame(width: 100)
                            }
                        }
                    } header: {
                        Text("Sensor")
                    }

                    // Night mode
                    Section {
                        Toggle(isOn: $settings.nightModeAuto) {
                            Label("Automatisch", systemImage: "moon.fill")
                        }
                        if settings.nightModeAuto {
                            HStack {
                                Label("Start", systemImage: "moon.zzz")
                                Spacer()
                                Text("\(settings.nightStartH):00")
                                    .monospacedDigit()
                                    .foregroundStyle(.secondary)
                                Stepper("", value: $settings.nightStartH, in: 0...23)
                                    .labelsHidden()
                                    .frame(width: 100)
                            }
                            HStack {
                                Label("Ende", systemImage: "sun.horizon")
                                Spacer()
                                Text("\(settings.nightEndH):00")
                                    .monospacedDigit()
                                    .foregroundStyle(.secondary)
                                Stepper("", value: $settings.nightEndH, in: 0...23)
                                    .labelsHidden()
                                    .frame(width: 100)
                            }
                            HStack {
                                Label("Helligkeit", systemImage: "sun.min")
                                Spacer()
                                Text("\(settings.nightBrightness)%")
                                    .monospacedDigit()
                                    .foregroundStyle(.secondary)
                                    .frame(width: 40, alignment: .trailing)
                            }
                            Slider(value: Binding(
                                get: { Double(settings.nightBrightness) },
                                set: { settings.nightBrightness = Int($0) }
                            ), in: 10...100, step: 5)
                            .tint(.orange)
                        }
                    } header: {
                        Text("Nachtmodus")
                    }

                    // Save button
                    Section {
                        Button {
                            Task { await save() }
                        } label: {
                            HStack {
                                Spacer()
                                if isSaving {
                                    ProgressView()
                                        .tint(.white)
                                } else {
                                    Image(systemName: "checkmark.circle.fill")
                                    Text("Einstellungen speichern")
                                        .fontWeight(.semibold)
                                }
                                Spacer()
                            }
                            .padding(.vertical, 4)
                        }
                        .listRowBackground(Color.blue)
                        .foregroundStyle(.white)
                        .disabled(isSaving)
                    }

                    if let msg = saveMessage {
                        Section {
                            HStack {
                                Image(systemName: msg.contains("Gespeichert") ? "checkmark.circle" : "xmark.circle")
                                    .foregroundStyle(msg.contains("Gespeichert") ? .green : .red)
                                Text(msg)
                                    .font(.callout)
                            }
                        }
                    }
                } else {
                    Section {
                        HStack {
                            Spacer()
                            ProgressView("Lade Einstellungen...")
                            Spacer()
                        }
                        .padding(.vertical, 40)
                    }
                }
            }
            .navigationTitle("Einstellungen")
            .task { await loadSettings() }
            .refreshable { await loadSettings() }
            .alert("Server URL", isPresented: $showServerEdit) {
                TextField("http://...", text: $bindableApi.serverURL)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                Button("OK") {
                    api.startPolling()
                    Task { await loadSettings() }
                }
                Button("Abbrechen", role: .cancel) {}
            } message: {
                Text("Gib die URL deines BME688 Web-Servers ein:")
            }
        }
    }

    private func loadSettings() async {
        isLoading = true
        if let s = await api.fetchSettings() {
            settings = s
        }
        isLoading = false
    }

    private func save() async {
        isSaving = true
        saveMessage = nil
        let ok = await api.saveSettings(settings)
        isSaving = false
        if ok {
            saveMessage = "Gespeichert! Wird an Pi gesendet..."
            // Reload to confirm round-trip
            try? await Task.sleep(for: .seconds(3))
            if let s = await api.fetchSettings() {
                settings = s
                saveMessage = "Gespeichert und bestaetigt!"
            }
            try? await Task.sleep(for: .seconds(2))
            saveMessage = nil
        } else {
            saveMessage = "Fehler beim Speichern"
        }
    }
}

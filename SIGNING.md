# Code signing

Releases are Authenticode-signed with **Azure Artifact Signing** (formerly Trusted Signing), so
Windows shows a verified publisher instead of "Unknown publisher". The same setup signs locally
(`build.bat sign`) and in GitHub Actions (push a `v*` tag).

- [sign.ps1](sign.ps1) signs any files you give it, using signtool and Microsoft's signing client.
- [signing/metadata.json](signing/metadata.json) holds the account details. They aren't secret.
- [.github/workflows/release.yml](.github/workflows/release.yml) builds, signs and publishes a release.

## One-time Azure setup

Cost: the Basic plan is $9.99/month for 5,000 signatures. Billing starts the day you create the
account and isn't prorated. It needs a pay-as-you-go subscription (free/trial credits don't work), and
the Azure billing account must be of type **Individual**, with your legal name and address exactly as
on your ID.

1. **Azure CLI** (for local signing): `winget install -e --id Microsoft.AzureCLI`, then `az login`.
2. **Register the provider**: `az provider register --namespace Microsoft.CodeSigning`
3. **Create the account**: Portal → *Artifact Signing Accounts* → *Create*. Pick a US region
   (e.g. West US 2) and the **Basic** tier, and note the account name and region.
4. **Give yourself the verifier role**: account → *Access control (IAM)* → *Add role assignment* →
   **Artifact Signing Identity Verifier** → yourself. Even the subscription owner needs this.
5. **Verify your identity**: account → *Identity validations* → set the dropdown to **Individual** →
   *New identity* → *Public*. Follow the emailed link and verify with Microsoft Authenticator and a
   government ID. It takes 1–20 business days; wait for **Completed**.
6. **Create the certificate profile**: account → *Certificate profiles* → *Create* → **Public Trust**,
   and pick your completed identity validation.
7. **Give yourself the signer role**: account → *Access control (IAM)* → **Artifact Signing Certificate
   Profile Signer** → yourself.
8. **Fill in [signing/metadata.json](signing/metadata.json)**: the endpoint for your region
   (`https://wus2.codesigning.azure.net` for West US 2, `https://eus.codesigning.azure.net` for East
   US, `https://wus.…`, `https://wus3.…`, `https://cus.…`, `https://scus.…`, `https://ncus.…`,
   `https://wcus.…`), the account name and the profile name. The endpoint must match the account's
   region, or signing fails with 403.

## Signing locally

```bat
az login
build.bat sign
```

The first run downloads Microsoft's signing client (NuGet `Microsoft.ArtifactSigning.Client`) to
`%LOCALAPPDATA%\ArtifactSigning` and checks Microsoft's signature on it. Needs Windows SDK signtool
10.0.22621 or later and the .NET 10 runtime.

## Signing in GitHub Actions

GitHub signs in to Azure with OIDC, so no password or client secret is stored anywhere.

1. **App registration**: Portal → *Microsoft Entra ID* → *App registrations* → *New registration*
   (e.g. `github-signing`). Note its *Application (client) ID* and *Directory (tenant) ID*.
2. **Federated credential**: the app → *Certificates & secrets* → *Federated credentials* → *Add* →
   *GitHub Actions deploying Azure resources*: organization `karlockhart`, repository
   `simple-crosshair`, entity type **Environment**, name **signing**.
   Add one per repository that signs; an app holds up to 20.
3. **Signer role for the app**: Artifact Signing account → *Access control (IAM)* →
   **Artifact Signing Certificate Profile Signer** → the `github-signing` app.
4. **GitHub environment**: repo *Settings* → *Environments* → *New environment* `signing`. Under
   *Deployment branches and tags*, allow only tags matching `v*`. Then add the secrets:

   ```bash
   gh secret set AZURE_CLIENT_ID --env signing -R karlockhart/simple-crosshair
   gh secret set AZURE_TENANT_ID --env signing -R karlockhart/simple-crosshair
   ```

5. **Release**: `git tag v0.2.0 && git push origin v0.2.0`

## Good to know

- Certificates last 3 days and renew on their own. The timestamp keeps old signatures valid.
- The SmartScreen "unrecognized app" prompt fades as a signed file collects downloads. It isn't
  instant.
- Every signed file counts against the 5,000/month quota.

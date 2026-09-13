# Security

- API keys are read **only** from environment variables (`FASTMM_*_API_KEY/SECRET`) via `${VAR}` substitution in TOML. Never commit keys; `.env` is git-ignored.
- The logger redacts values wrapped in `Secret<T>`; journals never contain credentials.
- Default configs point at exchange **testnets**. Live trading is at your own risk.
- Report vulnerabilities via GitHub private security advisories.

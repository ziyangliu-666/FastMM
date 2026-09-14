# Security

- API keys come from environment variables (`FASTMM_*_API_KEY/SECRET`) through `${VAR}` substitution in TOML; literal secrets are refused ([Configuration](docs/reference/configuration.md#general-rules)). Never commit keys; `.env` is git-ignored.
- The logger redacts values wrapped in `Secret<T>`; journals record the configuration without `api_key` and `api_secret`.
- The configs in `configs/` point at testnets, Binance Demo Mode or the local simulator. Live trading is at your own risk.
- Report vulnerabilities through GitHub private security advisories.

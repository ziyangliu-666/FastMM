def test_import():
    import fastmm

    assert fastmm.__version__.count(".") == 2
    assert "fastmm" in fastmm.build_info()

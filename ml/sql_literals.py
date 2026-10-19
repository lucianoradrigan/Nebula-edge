"""Literales SQL para los .sql que generan export_model.py y export_testset.py.

Los .sql se cargan con psql, sin parámetros, así que cada texto va escrito
dentro del INSERT. Un ' sin escapar (p.ej. en --trained-on "Paderborn's") corta
el literal y rompe el archivo, o peor, mete SQL.
"""


def sql_text(value: str | None) -> str:
    """'texto' con las comillas simples dobladas, o NULL si no hay valor."""
    if value is None:
        return "NULL"
    return "'" + str(value).replace("'", "''") + "'"

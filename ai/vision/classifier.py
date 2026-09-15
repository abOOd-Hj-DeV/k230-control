from typing import Dict
from config import NSFW_THRESHOLD


class Classifier:
    """يأخذ نتيجة النموذج ويحوّلها لقرار"""

    def __init__(self, threshold: float = NSFW_THRESHOLD):
        self.threshold = threshold

    def decide(self, scores: Dict[str, float]) -> Dict:
        nsfw = scores.get("nsfw", 0.0)
        if nsfw >= self.threshold:
            return {
                "action": "ALERT",
                "label": "NSFW",
                "confidence": nsfw,
                "scores": scores,
            }
        return {
            "action": "LOG",
            "label": "SAFE",
            "confidence": 1 - nsfw,
            "scores": scores,
        }